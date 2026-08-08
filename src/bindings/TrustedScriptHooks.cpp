#include "bindings/DomBindings.h"
#include "bindings/TrustedScript.h"

namespace microbrowser::bindings {

void DomBindings::WireTrustedScriptHooks() {
  if (interpreter_ == nullptr) {
    return;
  }
  InstallTrustedScriptSlot(*interpreter_, *this);
  interpreter_->SetTrustedScriptHooks(
      this,
      [](void* context) {
        return static_cast<DomBindings*>(context)->InTrustedScriptContext();
      },
      [](void* context, bool push) {
        auto* bindings = static_cast<DomBindings*>(context);
        if (push) {
          bindings->PushTrustedScriptContext();
        } else {
          bindings->PopTrustedScriptContext();
        }
      });
}

}  // namespace microbrowser::bindings
