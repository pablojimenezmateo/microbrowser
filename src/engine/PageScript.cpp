#include "engine/PageScript.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"
#include "util/Env.h"
#include "util/LoadTimeline.h"
#include "util/PerformanceTrace.h"

#include <cstdio>

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// The JavaScript MIME type essences, from the MIME Sniffing Standard's list.
//
// Sixteen strings and every one of them is history: `text/javascript1.3` and `text/livescript` name
// languages that have not existed for twenty-five years, and they are here because the list is
// *closed* -- a `type` that is not on it is not a script. That is the half this browser had
// backwards. It accepted three spellings and rejected the other thirteen, so
// `<script type="text/ecmascript">` was treated as data; and it accepted anything the list does not
// contain only by accident of the same short comparison.
constexpr std::string_view kJavaScriptMimeTypes[] = {
    "application/ecmascript",   "application/javascript",   "application/x-ecmascript",
    "application/x-javascript", "text/ecmascript",          "text/javascript",
    "text/javascript1.0",       "text/javascript1.1",       "text/javascript1.2",
    "text/javascript1.3",       "text/javascript1.4",       "text/javascript1.5",
    "text/jscript",             "text/livescript",          "text/x-ecmascript",
    "text/x-javascript",
};

// HTML's "script block's type string" (prepare the script element, step 8), which is *not* simply
// the `type` attribute:
//
//   type present     -> the attribute, stripped of leading and trailing whitespace; empty means
//                       `text/javascript`
//   language present -> `text/` followed by the attribute; empty means `text/javascript`
//   neither          -> `text/javascript`
//
// The `language` limb is why `<script language="JavaScript1.2">` runs and `<script
// type="javascript1.2">` does not: the first becomes `text/javascript1.2`, which is on the list,
// and the second is a bare word that is not. A rule that read only `type` gets both wrong, in
// opposite directions.
std::string ScriptTypeString(const dom::Element& element) {
  if (const std::string* type = element.GetAttribute("type"); type != nullptr) {
    const std::string trimmed(util::TrimHtmlWhitespace(*type));
    return trimmed.empty() ? std::string("text/javascript") : trimmed;
  }
  if (const std::string* language = element.GetAttribute("language"); language != nullptr) {
    return language->empty() ? std::string("text/javascript") : "text/" + *language;
  }
  return "text/javascript";
}

bool IsModule(const dom::Element& element) {
  // Case-insensitive, and only from the `type` attribute: `language="module"` is
  // `text/module`, which is not a script at all.
  const std::string* type = element.GetAttribute("type");
  return type != nullptr &&
         util::EqualsAsciiCaseInsensitive(util::TrimHtmlWhitespace(*type), "module");
}

// A `type` that is not JavaScript is data the page put in a script tag so the
// parser would leave it alone -- a template, a JSON blob -- and running it
// would be worse than ignoring it.
bool IsJavaScript(const dom::Element& element) {
  if (IsModule(element)) {
    return true;
  }
  const std::string type = ScriptTypeString(element);
  for (const std::string_view essence : kJavaScriptMimeTypes) {
    if (util::EqualsAsciiCaseInsensitive(type, essence)) {
      return true;
    }
  }
  return false;
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
  collected_scripts_.clear();
  scripts_requested_ = 0;
  errors_.clear();
  ran_ = false;
  timers_ = bindings::TimerQueue{};
  pending_imports_.clear();
  module_fetches_.clear();
  requested_modules_.clear();
  modules_.Clear();
  frames_ = bindings::AnimationFrames{};
  idle_ = bindings::IdleCallbacks{};
  performance_ = bindings::Performance{};
}

void PageScript::Collect(dom::Document& document, const DocumentPolicy& policy) {
  slots_.clear();
  pending_urls_.clear();
  pending_slots_.clear();
  collected_scripts_.clear();
  scripts_requested_ = 0;
  ran_ = false;
  CollectInserted(document, policy);
}

// One inline script, run **at the insertion** rather than at the turn boundary.
//
// TD-0059. HTML's "prepare the script element" executes an inline classic script during the
// insertion steps, so `document.body.appendChild(s)` is followed by a line that reads what it set.
// This engine collected inserted scripts and ran them on the loop's next turn, which no page can
// see and which a test asserts against on the very next line.
//
// Deliberately **only** the inline classic case. An external script has to be fetched and a module
// has to be graphed, and both finish on a later turn whatever this does -- so they keep the path
// they had, and this function answers false for them rather than pretending.
//
// The CSP check is the same one `CollectInserted` makes, in the same order, because it is the same
// decision: whether this text may run at all. What moved is only *when*.
bool PageScript::RunInsertedNow(const dom::Element& element, const DocumentPolicy& policy) {
  if (interpreter_ == nullptr || element.TagName() != "script" || !IsJavaScript(element) ||
      IsModule(element)) {
    return false;
  }
  const std::string* src = element.GetAttribute("src");
  if (src != nullptr && !src->empty()) {
    return false;
  }
  if (collected_scripts_.contains(&element)) {
    return false;  // already run, or already queued by a collection pass
  }
  const std::string text = element.TextContent();
  if (text.empty()) {
    return false;
  }
  // Marked before the policy check and before the run, so that neither a refusal nor a throw can
  // leave the element eligible for a second attempt on the next collection.
  collected_scripts_.insert(&element);
  const std::string* nonce_attribute = element.GetAttribute("nonce");
  const std::string_view nonce =
      nonce_attribute == nullptr ? std::string_view{} : std::string_view(*nonce_attribute);
  const bool trusted = bindings_ != nullptr && bindings_->IsCspTrustedScript(element);
  if (!trusted && !policy.AllowsInline(csp::Directive::Script, nonce, text)) {
    AddPerformanceCounter(PerfCounterId::CspInlineBlocked);
    return true;  // handled: refused, and it must not fall through to the deferred path
  }
  if (bindings_ != nullptr && (trusted || !nonce.empty())) {
    bindings_->MarkCspTrustedScript(element);
  }
  util::PerformanceTrace::ScopeLabel label("js::RunScript");
  label.Field("src", std::string("inserted")).Field("bytes", static_cast<long long>(text.size()));
  util::PerformanceTrace::Scope scope(label.View());
  const js::Result result = interpreter_->Run(text);
  if (result.completion == js::Completion::Throw) {
    // Recorded rather than propagated, exactly as a slot's throw is: a script that throws does not
    // stop the page, and a throw nobody wrote down is nine failed scripts looking like none.
    errors_.push_back("inserted script: " + js::ToString(result.value));
    interpreter_->ReportUncaught(result.value, "inserted script");
  }
  return true;
}

bool PageScript::CollectInserted(dom::Document& document, const DocumentPolicy& policy) {
  bool added = false;
  script_strict_dynamic_ = policy.ScriptStrictDynamic();
  eval_forbidden_ = !policy.AllowsEval();
  // Whether an `on*` content attribute may be compiled. Answered here, where
  // the policy is, and carried to the binding layer as a flag -- that layer
  // may not see `src/csp` (ADR 0008), and this is the one CSP question it has
  // to know the answer to. Re-asked on every collection, because a `<meta>`
  // policy can arrive after the first element did.
  inline_handlers_allowed_ = policy.AllowsInlineHandler();
  if (bindings_ != nullptr) {
    bindings_->SetScriptStrictDynamic(script_strict_dynamic_);
    bindings_->SetInlineHandlersAllowed(inline_handlers_allowed_);
  }
  if (interpreter_ != nullptr) {
    interpreter_->SetEvalForbidden(this, [](void* context) {
      return static_cast<PageScript*>(context)->eval_forbidden_;
    });
  }
  document.ForEachDescendant([this, &policy, &added](const dom::Node& node) {
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
    const std::string text = node.TextContent();
    if (!external && text.empty()) {
      return;
    }
    const bool seen = collected_scripts_.contains(&element);
    if (seen) {
      if (!external) {
        return;
      }
      const bool already_queued = std::any_of(
          pending_urls_.begin(), pending_urls_.end(),
          [&](const SubresourceRequest& request) { return request.url == *src; });
      if (already_queued) {
        return;
      }
    } else {
      collected_scripts_.insert(&element);
    }
    const std::string* nonce_attribute = element.GetAttribute("nonce");
    const std::string_view nonce =
        nonce_attribute == nullptr ? std::string_view{} : std::string_view(*nonce_attribute);
    const bool trusted =
        bindings_ != nullptr && bindings_->IsCspTrustedScript(element);

    if (external) {
      if (!trusted && !policy.AllowsUrl(csp::Directive::Script, *src, nonce)) {
        if (!seen) {
          collected_scripts_.erase(&element);
        }
        return;
      }
    } else if (!policy.AllowsInline(csp::Directive::Script, nonce, text)) {
      AddPerformanceCounter(PerfCounterId::CspInlineBlocked);
      if (!seen) {
        collected_scripts_.erase(&element);
      }
      return;
    }
    if (bindings_ != nullptr && (trusted || !nonce.empty())) {
      bindings_->MarkCspTrustedScript(element);
    }

    Slot slot;
    slot.timing = TimingFor(element, external, module);
    slot.module = module;
    slot.element = &element;
    if (!external) {
      slot.source = text;
      slots_.push_back(std::move(slot));
      added = true;
      return;
    }
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
    added = true;
  });
  return added;
}

std::vector<SubresourceRequest> PageScript::TakeUnrequestedScripts() {
  if (scripts_requested_ >= pending_urls_.size()) {
    return {};
  }
  std::vector<SubresourceRequest> pending(
      pending_urls_.begin() + static_cast<std::ptrdiff_t>(scripts_requested_), pending_urls_.end());
  scripts_requested_ = pending_urls_.size();
  return pending;
}

bool PageScript::RunPendingScripts() {
  if (interpreter_ == nullptr) {
    return false;
  }
  bool ran = RunTiming(Timing::Blocking);
  ran = RunTiming(Timing::Deferred) || ran;
  ran = RunTiming(Timing::Async) || ran;
  return ran;
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

void PageScript::NotifyFetchFailed(std::size_t index) {
  if (index >= pending_slots_.size() || bindings_ == nullptr) {
    return;
  }
  const std::size_t slot = pending_slots_[index];
  if (slot >= slots_.size()) {
    return;
  }
  if (const dom::Element* element = slots_[slot].element) {
    bindings_->NotifyScriptElementEvent(*element, "error");
  }
}

void PageScript::SetTrustedInsertionFlush(std::function<void(const dom::Element&)> hook) {
  trusted_insertion_flush_ = std::move(hook);
  if (bindings_ != nullptr) {
    bindings_->SetTrustedScriptFlush(trusted_insertion_flush_);
  }
}

void PageScript::SetActivationHooks(std::function<bool(dom::Element&)> pre_click,
                                    std::function<void()> cancel, std::function<void()> finish) {
  pre_click_activation_ = std::move(pre_click);
  cancel_activation_ = std::move(cancel);
  finish_activation_ = std::move(finish);
  if (bindings_ != nullptr) {
    bindings_->SetActivationHooks(pre_click_activation_, cancel_activation_, finish_activation_);
  }
}

void PageScript::EnsureInterpreter(dom::Document& document, const std::string& url,
                                   std::int64_t now_ms) {
  if (interpreter_ != nullptr) {
    return;
  }
  interpreter_ = std::make_unique<js::Interpreter>();
  bindings_ = std::make_unique<bindings::DomBindings>(*interpreter_, document, url,
                                                     geometry_, network_, history_, storage_,
                                                     cookies_, sockets_, media_, canvas_,
                                                     workers_, indexed_db_, animations_);
  bindings_->Install();
  bindings_->SetScriptStrictDynamic(script_strict_dynamic_);
  bindings_->SetInlineHandlersAllowed(inline_handlers_allowed_);
  if (pre_click_activation_ || cancel_activation_) {
    bindings_->SetActivationHooks(pre_click_activation_, cancel_activation_, finish_activation_);
  }
  if (trusted_insertion_flush_) {
    bindings_->SetTrustedScriptFlush(trusted_insertion_flush_);
  }
  timers_.Install(*interpreter_, now_ms);
  frames_.Install(*interpreter_, now_ms);
  idle_.Install(*interpreter_, now_ms);
  performance_.Install(*interpreter_, now_ms);
  interpreter_->SetEvalForbidden(this, [](void* context) {
    return static_cast<PageScript*>(context)->eval_forbidden_;
  });
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
    // Labelled with the script's own name and size, because "script is slow" is
    // not a finding on a page that serves twenty-six of them and one of them is
    // 10.7MB. ScopeLabel does the concatenation only when the channel is on.
    util::PerformanceTrace::ScopeLabel label("js::RunScript");
    label.Field("src", SourceName(slot))
        .Field("bytes", static_cast<long long>(source.size()));
    util::PerformanceTrace::Scope scope(label.View());
    // A script runs to completion inside one turn of the loop (TD-0007), so
    // this pair brackets a period during which nothing is drained and no frame
    // is presented. On the timeline that reads as a gap, which is what it is.
    if (util::LoadTimeline::Enabled()) {
      util::LoadTimeline::MarkWith("script.start", SourceName(slot));
    }
    // Same question the load-turn trace asks from outside: which script is
    // Advance stuck in? youtube.com under real querySelector (TD-0013) hangs
    // inside one Advance, and without this line the only evidence is a turn
    // that never returns.
    if (util::EnvFlagEnabled("MICROBROWSER_LOAD_TURN_TRACE")) {
      std::fprintf(stderr, "[load] script.start %s bytes=%zu\n", SourceName(slot).c_str(),
                   source.size());
      std::fflush(stderr);
    }
    const dom::Element* element = entry.element;
    if (bindings_ != nullptr) {
      // HTML §7.3.3's named access, refreshed here because this is the moment a
      // bare global can next be read. Gated on the document's mutation version,
      // so a page whose tree has not moved since the last script pays one
      // integer compare.
      bindings_->SyncNamedAccess();
      bindings_->SetTrustedScriptInsertion(true);
    }
    const js::Result result = entry.module
                                  ? interpreter_->RunModule(source, SourceName(slot))
                                  : interpreter_->Run(source);
    if (bindings_ != nullptr) {
      bindings_->SetTrustedScriptInsertion(false);
    }
    if (util::EnvFlagEnabled("MICROBROWSER_LOAD_TURN_TRACE")) {
      std::fprintf(stderr, "[load] script.end %s\n", SourceName(slot).c_str());
      std::fflush(stderr);
    }
    if (util::LoadTimeline::Enabled()) {
      util::LoadTimeline::MarkWith("script.end", SourceName(slot));
    }
    if (result.completion != js::Completion::Throw) {
      if (element != nullptr && bindings_ != nullptr) {
        // Fire `load` *before* any `data-loaded` bit. YouTube's script loader
        // (`P_U` / `XtI` / `_.VE`) registers waiters and on load runs
        // `BzU(el)||(hQn(el), OgC(...))`. `BzU` reads `dataset.loaded`. If we
        // stamped `data-loaded` first, that short-circuit skipped `OgC`, so
        // `EHT`'s `wja` never ran, `Application.create` was never called with a
        // target, and SPA watch left `create` defined but no `#movie_player`
        // (TD-0024). `hQn` sets the attribute itself when the completion runs.
        bindings_->NotifyScriptElementEvent(*element, "load");
      }
      continue;
    }
    // Root across `error` dispatch: that path drains microtasks and can collect
    // the thrown value. Reading `e.stack` afterwards was the youtube.com
    // segfault ValueRoot documents — Run() unroots on return, then we allocate.
    const js::Interpreter::ValueRoot rooted(*interpreter_, result.value);
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
    if (element != nullptr && bindings_ != nullptr) {
      bindings_->NotifyScriptElementEvent(*element, "error");
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

void PageScript::SetDocumentTiming(std::int64_t navigation_start_wall_ms, double response_end_ms) {
  // Same argument as above: held as plain data until there is a heap. This one
  // is always early -- the document arriving is what makes a script exist.
  performance_.SetDocumentTiming(interpreter_.get(), navigation_start_wall_ms, response_end_ms);
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

bool PageScript::NotifyWindowResize() {
  return bindings_ != nullptr && bindings_->NotifyWindowResize();
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
  const std::optional<std::uint32_t> idle = idle_.NextDelay(now_ms);
  std::optional<std::uint32_t> soonest;
  for (const std::optional<std::uint32_t>* candidate :
       {&timer, &frame, &idle}) {
    if (!candidate->has_value()) {
      continue;
    }
    soonest = soonest.has_value() ? std::optional<std::uint32_t>(
                                        std::min(*soonest, **candidate))
                                  : *candidate;
  }
  return soonest;
}

bool PageScript::RunDueWork(std::int64_t now_ms) {
  if (interpreter_ != nullptr) {
    performance_.Tick(*interpreter_, now_ms);
  }
  if (interpreter_ == nullptr) {
    return false;
  }
  // Timers first, then the frame, then idle callbacks. That is the order the
  // event loop defines: a timer that moves something should be reflected by the
  // frame that draws it, and idle work runs only after both.
  const bool timers = timers_.RunDue(*interpreter_, now_ms);
  const bool frame = frames_.RunDue(*interpreter_, now_ms);
  const bool idle = idle_.RunDue(*interpreter_, now_ms);
  return timers || frame || idle;
}

bool PageScript::DispatchSubmit(dom::Element& form) {
  return bindings_ != nullptr && bindings_->DispatchSubmit(form);
}

bool PageScript::DispatchInput(dom::Element& target) {
  return bindings_ != nullptr && bindings_->DispatchInput(target);
}

std::optional<bindings::PendingSubmit> PageScript::TakePendingSubmit() {
  return bindings_ == nullptr ? std::nullopt : bindings_->TakePendingSubmit();
}

std::vector<dom::Element*> PageScript::TakePendingActivations() {
  return bindings_ == nullptr ? std::vector<dom::Element*>{}
                              : bindings_->TakePendingActivations();
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

bool PageScript::DispatchPointerMouse(dom::Element& target, std::string_view type,
                                      const bindings::PointerInput& pointer) {
  return bindings_ != nullptr && bindings_->DispatchPointerMouse(target, type, pointer);
}

void PageScript::NotifyElementEvent(const dom::Element& element, const char* type) {
  if (bindings_ != nullptr) {
    bindings_->NotifyScriptElementEvent(element, type);
  }
}

bool PageScript::DispatchKey(dom::Node* target, const bindings::KeyInput& key) {
  return bindings_ != nullptr && bindings_->DispatchKey(target, key);
}

bool PageScript::MoveFocus(dom::Element* target, bool visible) {
  return bindings_ != nullptr && bindings_->MoveFocus(target, visible);
}

std::string PageScript::Evaluate(dom::Document& document, const std::string& url,
                                 std::string_view source) {
  EnsureInterpreter(document, url, 0);
  if (interpreter_ == nullptr) {
    return {};
  }
  const js::Result result = interpreter_->Run(source);
  // Root across this second drain: Run() already drained once under a ValueRoot
  // that ended when Run returned. Same shape as the script-error UAF above.
  const js::Interpreter::ValueRoot rooted(*interpreter_, result.value);
  // Microtasks too, so `await`-shaped probes and a promise a probe resolves
  // settle before the answer is read -- which is what makes asking about
  // anything asynchronous possible at all.
  interpreter_->DrainMicrotasks();
  if (result.completion == js::Completion::Throw) {
    return "throw " + js::ToString(result.value);
  }
  return js::ToString(result.value);
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
  // And `matchMedia`, at the same one place and for the same reason: the
  // browser is the only thing that knows the viewport moved, and a page that
  // could make a `change` fire would be one that could run its own resize
  // handlers on demand. It is last because a handler that rearranges the page
  // wants the geometry observers to see the result on the next pass rather
  // than half of it on this one.
  ran = bindings_->DeliverMediaQueryChanges() || ran;
  return ran;
}

const std::vector<std::string>& PageScript::ConsoleOutput() const {
  static const std::vector<std::string> kNone;
  return interpreter_ == nullptr ? kNone : interpreter_->ConsoleOutput();
}

}  // namespace microbrowser::engine
