#include "bindings/TrustedScript.h"

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

namespace microbrowser::bindings {

namespace {

DomBindings* BindingsOf(js::Interpreter& interpreter) {
  const js::Value* slot = interpreter.Global()->GetOwn(kTrustedScriptBindingsSlot);
  if (slot == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<DomBindings*>(static_cast<std::uintptr_t>(slot->number));
}

}  // namespace

void InstallTrustedScriptSlot(js::Interpreter& interpreter, DomBindings& bindings) {
  interpreter.Global()->Set(kTrustedScriptBindingsSlot, PointerValue(&bindings));
}

bool TrustedScriptContextActive(js::Interpreter& interpreter) {
  DomBindings* bindings = BindingsOf(interpreter);
  return bindings != nullptr && bindings->InTrustedScriptContext();
}

void PushTrustedScriptContext(js::Interpreter& interpreter) {
  if (DomBindings* bindings = BindingsOf(interpreter)) {
    bindings->PushTrustedScriptContext();
  }
}

void PopTrustedScriptContext(js::Interpreter& interpreter) {
  if (DomBindings* bindings = BindingsOf(interpreter)) {
    bindings->PopTrustedScriptContext();
  }
}

TrustedScriptInvocation::TrustedScriptInvocation(js::Interpreter& interpreter, bool enabled)
    : interpreter_(interpreter), enabled_(enabled) {
  if (enabled_) {
    PushTrustedScriptContext(interpreter_);
  }
}

TrustedScriptInvocation::~TrustedScriptInvocation() {
  if (enabled_) {
    PopTrustedScriptContext(interpreter_);
  }
}

}  // namespace microbrowser::bindings
