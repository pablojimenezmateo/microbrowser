#pragma once

#include "js/Interpreter.h"

namespace microbrowser::bindings {

class DomBindings;

// Where async callback paths find the binding layer to ask whether a `<script>`
// insert is CSP-trusted. reddit's concat polyfill appends tags from fetch
// continuations and timer callbacks, not only from synchronous script bodies.
inline constexpr const char* kTrustedScriptBindingsSlot = "#trustedScriptBindings";

void InstallTrustedScriptSlot(js::Interpreter& interpreter, DomBindings& bindings);
bool TrustedScriptContextActive(js::Interpreter& interpreter);
void PushTrustedScriptContext(js::Interpreter& interpreter);
void PopTrustedScriptContext(js::Interpreter& interpreter);

// Enables trusted `<script>` insertion for one host callback when `enabled`.
struct TrustedScriptInvocation {
  TrustedScriptInvocation(js::Interpreter& interpreter, bool enabled);
  ~TrustedScriptInvocation();
  TrustedScriptInvocation(const TrustedScriptInvocation&) = delete;
  TrustedScriptInvocation& operator=(const TrustedScriptInvocation&) = delete;

 private:
  js::Interpreter& interpreter_;
  bool enabled_ = false;
};

}  // namespace microbrowser::bindings
