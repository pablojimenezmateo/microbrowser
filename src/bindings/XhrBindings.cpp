// `XMLHttpRequest`, expressed in terms of the session-13 machinery rather than
// beside it.
//
// ADR 0020 §1 is explicit that there is one request path and `fetch` is the
// shape of it: the older API is a shim over the newer one, so that a page using
// either passes the same privacy verdict, the same CORS check, the same
// `connect-src` and the same connection pool. There is deliberately no second
// entry point into `NetworkSource` -- an XHR is a `ScriptRequest` like any other
// and its answer arrives through the same pending table, which is why
// `DeliverFetchResponse` decides between a promise and an XHR rather than there
// being two deliveries.
//
// Two things here are absences rather than omissions, per ADR 0012:
//
//   * **A synchronous XHR throws.** `open(..., false)` is an
//     `InvalidAccessError`, because this browser has one loop and blocking it is
//     what ADR 0011 exists to prevent. A page that feature-detects gets a
//     legible failure at the call; a stub that quietly ran asynchronously would
//     return with `responseText` empty and the page would carry on as if it had
//     data.
//   * **`timeout`, `upload`, `overrideMimeType` and the non-text response types
//     are not defined at all.** Each of them is a property a page sets and then
//     trusts. A `timeout` that never fired, or a `responseType =
//     "arraybuffer"` that answered with a string, is worse than the name being
//     missing -- which is the whole of ADR 0012's rule.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "url/Url.h"
#include "bindings/FetchSupport.h"
#include "bindings/Network.h"
#include "html/Encoding.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;
using util::AddPerformanceCounter;
using util::PerfCounterId;

// The request being assembled, and the answer once it arrives. Every one of
// these is a property on the XHR object rather than a C++ field, for the reason
// the pending fetch table is a JavaScript array: the collector cannot see a
// `js::Value` in a C++ member, and this module has had that bug once already.
// `kXhrSlot` itself is in FetchSupport.h, because the delivery reads it.
constexpr const char* kXhrMethodSlot = "#xhr:method";
constexpr const char* kXhrUrlSlot = "#xhr:url";
constexpr const char* kXhrHeadersSlot = "#xhr:requestHeaders";
constexpr const char* kXhrSentSlot = "#xhr:sent";
constexpr const char* kXhrResponseHeadersSlot = "#xhr:responseHeaders";
constexpr const char* kXhrResponseTypeSlot = "#xhr:responseType";

// The five readyState values, spelled out because a page compares against the
// constants and against the numbers interchangeably.
constexpr double kUnsent = 0;
constexpr double kOpened = 1;
constexpr double kHeadersReceived = 2;
constexpr double kLoading = 3;
constexpr double kDone = 4;

double ReadyStateOf(const Value& xhr) {
  const Value* state = xhr.IsObject() ? xhr.object->GetOwn("readyState") : nullptr;
  return state == nullptr ? kUnsent : js::ToNumber(*state);
}

// `JSON.parse(text)`, or null when it throws -- which is what
// `xhr.responseType = "json"` is defined to answer for a body that does not
// parse. Through the page's own `JSON`, because there is exactly one JSON parser
// here and a second one in C++ would be a second set of answers about what a
// number is.
js::Value ParseJsonThrough(js::Interpreter& interpreter, std::string_view text) {
  // Through the global *scope* and not the global object: the builtins are
  // declared as scope bindings, which is where `JSON` actually is.
  const Value* json = interpreter.GlobalScope()->Lookup("JSON");
  if (json == nullptr || !json->IsObject()) {
    return Value::Null();
  }
  const Value* parse = json->object->Get("parse");
  if (parse == nullptr) {
    return Value::Null();
  }
  const js::Result result =
      interpreter.CallFunction(*parse, *json, {Value::String(std::string(text))});
  return result.completion == js::Completion::Throw ? Value::Null() : result.value;
}

// XHR's text decoder: the charset of `Content-Type`, or UTF-8. Not the document
// sniffer -- that one falls through to windows-1252, and a JSON endpoint with
// no charset would become mojibake rather than a parse error.
std::string DecodeXhrBody(const ScriptResponse& response) {
  html::Encoding encoding = html::Encoding::Utf8;
  for (const ScriptHeader& header : response.headers) {
    if (util::EqualsAsciiCaseInsensitive(header.name, "content-type")) {
      if (const std::optional<html::Encoding> found = html::EncodingFromMimeType(header.value)) {
        encoding = *found;
      }
      break;
    }
  }
  return html::DecodeToUtf8(response.body, encoding);
}

}  // namespace

void DomBindings::InstallXhr() {
  if (network_ == nullptr) {
    // No network behind this layer, so no `XMLHttpRequest`. The same absence
    // `fetch` gets, and for the stronger reason: a page that finds the
    // constructor and gets nothing back has no fallback path at all.
    return;
  }
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return;
  }
  const Value prototype = interpreter_->NewObjectValue();
  if (!prototype.IsObject()) {
    return;
  }
  interfaces_.object->Set("XMLHttpRequest", prototype);

  // --- open -----------------------------------------------------------------
  const Value open = interpreter_->NewNativeValue("open", [this](NativeCall& call) {
    if (!call.self.IsObject() || call.self.object->GetOwn(kXhrSlot) == nullptr) {
      return call.Throw("TypeError", "open called on something that is not an XHR");
    }
    if (call.arguments.size() >= 3 && !js::ToBoolean(call.arguments[2])) {
      // See the note at the top of this file. One loop, and a page that gets a
      // throw here can fall back; a page that gets a lie cannot.
      return ThrowDom(call, "InvalidAccessError",
                      "synchronous XMLHttpRequest is not supported");
    }
    std::string method;
    if (!CoerceToString(call, Argument(call.arguments, 0), method)) {
      return call.ThrownValue();
    }
    call.self.object->SetHidden(kXhrMethodSlot,
                                Value::String(method.empty() ? std::string("GET") : method));
    std::string url;
    if (!CoerceToString(call, Argument(call.arguments, 1), url)) {
      return call.ThrownValue();
    }
    if (url == "[object Object]") {
      return call.Throw("TypeError", "Failed to parse URL from [object Object]");
    }
    // The URL is parsed here rather than at `send()`, and a failure is a `SyntaxError` rather than
    // a request that quietly never happens. That is what the standard says, and the reason it says
    // it is that `open()` is where a page can still do something about it -- by the time `send()`
    // returns, the fallback path is gone.
    {
      const std::optional<url::Url> base = url::Url::Parse(DocumentBaseUrl(DocumentOf(call.self)));
      const std::optional<url::Url> parsed =
          base.has_value() ? url::Url::Parse(url, *base) : url::Url::Parse(url);
      if (!parsed.has_value()) {
        return ThrowDom(call, "SyntaxError", "Failed to parse URL: " + url);
      }
      url = parsed->Serialize();
    }
    call.self.object->SetHidden(kXhrUrlSlot, Value::String(url));
    // A re-open discards whatever the last exchange left behind. Without this a
    // reused XHR would answer with the previous response's headers.
    call.self.object->SetHidden(kXhrHeadersSlot, call.interpreter.NewArrayValue({}));
    call.self.object->SetHidden(kXhrResponseHeadersSlot, call.interpreter.NewArrayValue({}));
    call.self.object->SetHidden(kXhrSentSlot, Value::Bool(false));
    call.self.object->Set("status", Value::Number(0));
    call.self.object->Set("statusText", Value::String(""));
    call.self.object->Set("responseText", Value::String(""));
    call.self.object->Set("responseURL", Value::String(""));
    call.self.object->Set("response", Value::String(""));
    // Last, because everything above is what OPENED means: a page whose
    // `readystatechange` handler reads `status` must not see the previous
    // exchange's.
    AdvanceXhrState(call.self, kOpened);
    return Value::Undefined();
  });

  // --- setRequestHeader -----------------------------------------------------
  const Value set_request_header =
      interpreter_->NewNativeValue("setRequestHeader", [](NativeCall& call) {
        if (!call.self.IsObject() || call.self.object->GetOwn(kXhrSlot) == nullptr) {
          return call.Throw("TypeError", "setRequestHeader called on a non-XHR");
        }
        if (ReadyStateOf(call.self) != kOpened) {
          // The specification's rule, and it catches a real mistake: a header
          // set before `open` would be silently dropped by the re-open above.
          Value error = call.interpreter.MakeError(
              "Error", "setRequestHeader must be called after open and before send");
          if (error.IsObject()) {
            error.object->Set("name", Value::String("InvalidStateError"));
          }
          return call.ThrowValue(error);
        }
        const std::string name = LowerCase(js::ToString(Argument(call.arguments, 0)));
        if (IsForbiddenHeaderName(name)) {
          // Dropped silently, which is what the specification says: a page that
          // sets `Host` gets no error and no header.
          return Value::Undefined();
        }
        const Value* pairs = call.self.object->GetOwn(kXhrHeadersSlot);
        if (pairs != nullptr && pairs->IsObject()) {
          pairs->object->PushElement(
              MakePair(call.interpreter, name, js::ToString(Argument(call.arguments, 1))));
        }
        return Value::Undefined();
      });

  // --- send -----------------------------------------------------------------
  const Value send = interpreter_->NewNativeValue("send", [this](NativeCall& call) {
    if (!call.self.IsObject() || call.self.object->GetOwn(kXhrSlot) == nullptr) {
      return call.Throw("TypeError", "send called on something that is not an XHR");
    }
    const Value* sent = call.self.object->GetOwn(kXhrSentSlot);
    if (ReadyStateOf(call.self) != kOpened || (sent != nullptr && js::ToBoolean(*sent))) {
      Value error = call.interpreter.MakeError("Error", "send requires an opened XMLHttpRequest");
      if (error.IsObject()) {
        error.object->Set("name", Value::String("InvalidStateError"));
      }
      return call.ThrowValue(error);
    }

    ScriptRequest request;
    const Value* method = call.self.object->GetOwn(kXhrMethodSlot);
    const Value* url = call.self.object->GetOwn(kXhrUrlSlot);
    request.method = method == nullptr ? "GET" : js::ToString(*method);
    request.url = url == nullptr ? "" : js::ToString(*url);
    for (const Value& pair : ReadPairs(call.self, kXhrHeadersSlot)) {
      request.headers.push_back(ScriptHeader{PairPart(pair, 0), PairPart(pair, 1)});
    }
    // GET and HEAD have no request body. The specification sets body to null
    // before extract-MIME, so neither the bytes nor a Content-Type derived from
    // them go out -- an author-set Content-Type is kept.
    const bool bodyless = util::EqualsAsciiCaseInsensitive(request.method, "GET") ||
                          util::EqualsAsciiCaseInsensitive(request.method, "HEAD");
    const Value body = Argument(call.arguments, 0);
    if (!bodyless && !body.IsUndefined() && !body.IsNull()) {
      const bool had_content_type = std::any_of(
          request.headers.begin(), request.headers.end(), [](const ScriptHeader& header) {
            return util::EqualsAsciiCaseInsensitive(header.name, "content-type");
          });
      bool from_string = false;
      std::string extracted_type;
      if (!ExtractRequestBody(body, request.body, from_string, &extracted_type)) {
        return call.Throw("TypeError", "failed to read request body");
      }
      request.body_from_string = from_string;
      MaybeSetBodyContentType(request.headers, extracted_type, from_string);
      if (had_content_type && (from_string || IsSearchParamsValue(body))) {
        ReplaceAuthorContentTypeCharset(request.headers);
      }
    }
    // `withCredentials` is the whole of XHR's CORS surface, and it means what
    // `credentials: "include"` means -- which is why it can be one line here
    // rather than a second credentials model.
    const Value* with_credentials = call.self.object->Get("withCredentials");
    request.credentials =
        with_credentials != nullptr && js::ToBoolean(*with_credentials) ? "include" : "same-origin";

    call.self.object->SetHidden(kXhrSentSlot, Value::Bool(true));
    const std::uint64_t id = network_->StartFetch(request);
    if (id == 0) {
      // Refused before it started: an unparseable URL, a scheme this browser
      // will not fetch, or `connect-src`. A network error, which for an XHR is
      // `status` 0 and an `error` event -- and deliberately not an exception,
      // because a page cannot tell those three apart in any browser.
      FailXhr(call.self);
      return Value::Undefined();
    }
    AddPerformanceCounter(PerfCounterId::XhrRequests);

    // The same pending table a `fetch` uses, with the XHR where the promise
    // would be. One table, so that a navigation clearing the requests clears
    // both kinds and there is no second place to forget.
    const Value entry = call.interpreter.NewObjectValue();
    if (entry.IsObject()) {
      entry.object->SetHidden(kFetchIdSlot, Value::Number(static_cast<double>(id)));
      entry.object->SetHidden(kXhrSlot, call.self);
      const Value pending = PendingFetches();
      if (pending.IsObject()) {
        pending.object->PushElement(entry);
      }
    }
    return Value::Undefined();
  });

  // --- abort ----------------------------------------------------------------
  const Value abort = interpreter_->NewNativeValue("abort", [this](NativeCall& call) {
    if (!call.self.IsObject() || call.self.object->GetOwn(kXhrSlot) == nullptr) {
      return call.Throw("TypeError", "abort called on something that is not an XHR");
    }
    AbortXhr(call.self);
    return Value::Undefined();
  });

  // --- the response readers -------------------------------------------------
  const Value get_response_header =
      interpreter_->NewNativeValue("getResponseHeader", [](NativeCall& call) {
        const std::string wanted = LowerCase(js::ToString(Argument(call.arguments, 0)));
        for (const Value& pair : ReadPairs(call.self, kXhrResponseHeadersSlot)) {
          if (LowerCase(PairPart(pair, 0)) == wanted) {
            return Value::String(PairPart(pair, 1));
          }
        }
        // Null and not undefined: a page tests `!== null`.
        return Value::Null();
      });

  const Value get_all_response_headers =
      interpreter_->NewNativeValue("getAllResponseHeaders", [](NativeCall& call) {
        // CRLF-separated `name: value`, which is the format the specification
        // defines and which every library that parses this expects. The list is
        // already whatever this request is *allowed* to read: a cross-origin
        // response arrives with only the safelisted fields plus whatever
        // `Access-Control-Expose-Headers` named, because `net` removed the rest
        // before the response reached this module.
        std::string out;
        for (const Value& pair : ReadPairs(call.self, kXhrResponseHeadersSlot)) {
          out += LowerCase(PairPart(pair, 0));
          out += ": ";
          out += PairPart(pair, 1);
          out += "\r\n";
        }
        return Value::String(std::move(out));
      });

  // `responseType` is an accessor and not a plain property, because the setter
  // is where "only what this browser can honour" lives. An unrecognised value
  // is *ignored* -- the IDL enum's own behaviour -- so `responseType` reads back
  // as `""` and a page that asked for `arraybuffer` can see it did not get it.
  // A property that accepted the word and answered with a string would be the
  // stub ADR 0012 forbids.
  const Value response_type_get =
      interpreter_->NewNativeValue("responseType", [](NativeCall& call) {
        const Value* stored =
            call.self.IsObject() ? call.self.object->GetOwn(kXhrResponseTypeSlot) : nullptr;
        return stored == nullptr ? Value::String("") : *stored;
      });
  const Value response_type_set =
      interpreter_->NewNativeValue("responseType", [](NativeCall& call) {
        const std::string wanted = js::ToString(Argument(call.arguments, 0));
        if (call.self.IsObject() && (wanted.empty() || wanted == "text" || wanted == "json")) {
          call.self.object->SetHidden(kXhrResponseTypeSlot, Value::String(wanted));
        }
        return Value::Undefined();
      });
  if (response_type_get.IsObject() && response_type_set.IsObject()) {
    prototype.object->DefineAccessor("responseType", response_type_get.object,
                                     response_type_set.object);
  }

  for (const auto& [name, method] :
       std::initializer_list<std::pair<const char*, Value>>{
           {"open", open},
           {"setRequestHeader", set_request_header},
           {"send", send},
           {"abort", abort},
           {"getResponseHeader", get_response_header},
           {"getAllResponseHeaders", get_all_response_headers}}) {
    if (method.IsObject()) {
      method.object->Set(kOwnerSlot, OwnerValue(this));
      prototype.object->Set(name, method);
    }
  }
  // The readyState constants live on the prototype as well as on the
  // constructor, because a page reads `xhr.DONE` as often as
  // `XMLHttpRequest.DONE`.
  const auto add_constants = [](const Value& target) {
    if (!target.IsObject()) {
      return;
    }
    target.object->Set("UNSENT", Value::Number(kUnsent));
    target.object->Set("OPENED", Value::Number(kOpened));
    target.object->Set("HEADERS_RECEIVED", Value::Number(kHeadersReceived));
    target.object->Set("LOADING", Value::Number(kLoading));
    target.object->Set("DONE", Value::Number(kDone));
  };
  add_constants(prototype);

  const Value constructor =
      interpreter_->NewNativeValue("XMLHttpRequest", [this, prototype](NativeCall& call) {
        const Value xhr = call.interpreter.NewObjectValue();
        if (!xhr.IsObject()) {
          return Value::Undefined();
        }
        xhr.object->SetPrototype(prototype.object);
        xhr.object->SetHidden(kXhrSlot, Value::Bool(true));
        xhr.object->Set("readyState", Value::Number(kUnsent));
        xhr.object->Set("status", Value::Number(0));
        xhr.object->Set("statusText", Value::String(""));
        xhr.object->Set("responseText", Value::String(""));
        xhr.object->Set("responseURL", Value::String(""));
        xhr.object->Set("response", Value::String(""));
        xhr.object->Set("withCredentials", Value::Bool(false));
        xhr.object->SetHidden(kXhrHeadersSlot, call.interpreter.NewArrayValue({}));
        xhr.object->SetHidden(kXhrResponseHeadersSlot, call.interpreter.NewArrayValue({}));
        xhr.object->SetHidden(kXhrSentSlot, Value::Bool(false));
        // An event target, so `xhr.addEventListener('load', …)` works -- which
        // is how every wrapper written since 2010 listens for one.
        InstallEventMethods(xhr);
        return xhr;
      });
  if (constructor.IsObject()) {
    constructor.object->Set(kOwnerSlot, OwnerValue(this));
    // **The prototype has to be reachable from the constructor**, not only
    // installed on each instance. `XMLHttpRequest.prototype` is how a page
    // patches every request it will ever make -- and how it feature-detects:
    // youtube's bundle reads `XMLHttpRequest.prototype.fetch` to decide which
    // of two transports to use, and took a TypeError on `undefined.fetch`.
    // `instanceof` needs the same property.
    constructor.object->Set("prototype", prototype);
    prototype.object->Set("constructor", constructor);
    add_constants(constructor);
    interpreter_->Global()->Set("XMLHttpRequest", constructor);
    interpreter_->GlobalScope()->Declare("XMLHttpRequest", constructor, false);
  }
}

void DomBindings::AdvanceXhrState(const js::Value& xhr, double state) {
  if (interpreter_ == nullptr || !xhr.IsObject()) {
    return;
  }
  xhr.object->Set("readyState", Value::Number(state));
  const Value event = MakeEvent("readystatechange", /*bubbles=*/false, /*cancelable=*/false,
                                /*trusted=*/true);
  if (event.IsObject()) {
    event.object->Set("target", xhr);
    RunListenersOn(xhr, event, "#on:readystatechange", EventPhase::AtTarget);
  }
}

void DomBindings::FireXhrEvent(const js::Value& xhr, const char* type) {
  if (interpreter_ == nullptr || !xhr.IsObject()) {
    return;
  }
  const Value event = MakeEvent(type, /*bubbles=*/false, /*cancelable=*/false, /*trusted=*/true);
  if (event.IsObject()) {
    event.object->Set("target", xhr);
    RunListenersOn(xhr, event, std::string("#on:") + type, EventPhase::AtTarget);
  }
}

void DomBindings::FailXhr(const js::Value& xhr) {
  if (!xhr.IsObject()) {
    return;
  }
  // Status stays zero and the body stays empty. A network failure tells a page
  // nothing else -- which is the same rule `fetch` follows, and for the same
  // reason: an error that distinguished "no Access-Control-Allow-Origin" from
  // "connection refused" would be the cross-origin read CORS exists to stop.
  AdvanceXhrState(xhr, kDone);
  FireXhrEvent(xhr, "error");
  FireXhrEvent(xhr, "loadend");
  AddPerformanceCounter(PerfCounterId::XhrFailed);
  interpreter_->DrainMicrotasks();
}

void DomBindings::AbortXhr(const js::Value& xhr) {
  if (!xhr.IsObject() || interpreter_ == nullptr) {
    return;
  }
  const Value* pending =
      interfaces_.IsObject() ? interfaces_.object->GetOwn(kPendingFetchSlot) : nullptr;
  bool was_in_flight = false;
  if (pending != nullptr && pending->IsObject()) {
    std::vector<Value> kept;
    for (std::size_t i = 0; i < pending->object->ElementCount(); ++i) {
      const Value entry = pending->object->GetElement(i);
      const Value* entry_xhr = entry.IsObject() ? entry.object->GetOwn(kXhrSlot) : nullptr;
      if (entry_xhr == nullptr || !entry_xhr->IsObject() || entry_xhr->object != xhr.object) {
        kept.push_back(entry);
        continue;
      }
      was_in_flight = true;
      if (network_ != nullptr) {
        if (const Value* id = entry.object->GetOwn(kFetchIdSlot)) {
          network_->AbortFetch(static_cast<std::uint64_t>(js::ToNumber(*id)));
        }
      }
    }
    pending->object->SetElements(kept, std::vector<bool>(kept.size(), true));
  }
  if (!was_in_flight) {
    // `abort()` on an XHR that was never sent, or has already finished, resets
    // it and fires nothing. Firing `abort` there would run a page's cleanup
    // handler twice.
    xhr.object->Set("readyState", Value::Number(kUnsent));
    return;
  }
  xhr.object->Set("status", Value::Number(0));
  xhr.object->Set("responseText", Value::String(""));
  AdvanceXhrState(xhr, kDone);
  FireXhrEvent(xhr, "abort");
  FireXhrEvent(xhr, "loadend");
  // Back to UNSENT after the events, which is the specification's order and
  // what makes a handler see DONE rather than a state it never asked about.
  xhr.object->Set("readyState", Value::Number(kUnsent));
  AddPerformanceCounter(PerfCounterId::XhrAborted);
  interpreter_->DrainMicrotasks();
}

void DomBindings::DeliverToXhr(const js::Value& xhr, const ScriptResponse& response) {
  if (interpreter_ == nullptr || !xhr.IsObject()) {
    return;
  }
  if (!response.ok) {
    FailXhr(xhr);
    return;
  }
  std::vector<Value> pairs;
  pairs.reserve(response.headers.size());
  for (const ScriptHeader& header : response.headers) {
    pairs.push_back(MakePair(*interpreter_, LowerCase(header.name), header.value));
  }
  WritePairs(*interpreter_, xhr, kXhrResponseHeadersSlot, std::move(pairs));
  xhr.object->Set("status", Value::Number(static_cast<double>(response.status)));
  xhr.object->Set("statusText", Value::String(response.status_text));
  xhr.object->Set("responseURL", Value::String(response.url));
  const std::string text = DecodeXhrBody(response);
  xhr.object->Set("responseText", Value::String(text));

  // `response` is `responseText` unless the page asked for JSON, in which case
  // it is the parsed value -- and `null` when the body does not parse, which is
  // what the specification says and is why this cannot simply throw.
  const Value* response_type = xhr.object->GetOwn(kXhrResponseTypeSlot);
  const std::string wanted = response_type == nullptr ? "" : js::ToString(*response_type);
  if (wanted == "json") {
    xhr.object->Set("response", ParseJsonThrough(*interpreter_, response.body));
  } else {
    xhr.object->Set("response", Value::String(text));
  }

  // The three states in one turn, because a response arrives here whole: there
  // is no incremental parse or paint yet (see CLAUDE.md), so there is no point
  // at which headers are known and the body is not. A page that watches
  // `readyState` still sees every value in order, which is what it is watching
  // for.
  AdvanceXhrState(xhr, kHeadersReceived);
  AdvanceXhrState(xhr, kLoading);
  AdvanceXhrState(xhr, kDone);
  FireXhrEvent(xhr, "load");
  FireXhrEvent(xhr, "loadend");
  AddPerformanceCounter(PerfCounterId::XhrDelivered);
  interpreter_->DrainMicrotasks();
}

}  // namespace microbrowser::bindings
