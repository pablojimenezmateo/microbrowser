#include <memory>
#include <optional>
#include <vector>

#include "js/Interpreter.h"
#include "util/PerformanceCounters.h"

// Making a realm, and switching between them. ADR 0042.
//
// Split out of Interpreter.cpp for the reason the architecture lint gives when a
// file goes over its cap: this is a subject rather than more of that one. What is
// here is every place the *set* of realms or the *current* realm changes, which is
// four functions and a guard -- and having them in one file is what makes "who can
// move the running realm?" a question with a readable answer. `js/Realm.h` is the
// model those four functions maintain.
//
// The invariant worth stating once, because three of the four exist to keep it:
// **the running realm is the realm of the innermost callee, and the realm a
// top-level program belongs to is the realm the host entered.** Those are two
// different facts and they need two fields -- `current_realm_` follows calls and
// `host_realm_` does not. Collapsing them was a null dereference on the way into a
// program frame and a program silently continuing in its callee's realm on the way
// out; see `Interpreter::SyncRealm`.

namespace microbrowser::js {

// **The bound in js/Realm.h is on realms that exist at once, and without this it
// was a bound on realms ever made.** `url/failure.html` appends an `<iframe>`,
// reads it, and removes it, 188 times: one live frame throughout, and past the
// 64th every one of them silently ran no script. A page cannot hold more
// contexts than it has elements, so counting the dead ones measured the wrong
// thing.
//
// The slot is repopulated on reuse -- a fresh global, a fresh global scope, fresh
// intrinsics -- so nothing of the retired document reaches the next one. What is
// *not* recovered is an object the retired realm allocated that a page still
// holds: it records this id, so a callable among them would run against the new
// realm's global. That is bounded by what a realm is only ever created for -- a
// **same-origin** child of this document -- so it is one same-origin document's
// function reaching another's global, never a cross-origin one. Recovering it
// properly is per-realm collection, which ADR 0042 considered and rejected:
// same-origin realms hold references to each other by design, so the collector
// cannot treat either as independently reachable.
void Interpreter::RetireRealm(RealmId realm) {
  if (realm == kMainRealm || realm >= realms_.size()) {
    // Realm 0 is the interpreter's own and outlives everything; an id nobody
    // handed out cannot be given back. Both are no-ops rather than errors,
    // because the caller is a teardown path and a teardown that can fail is one
    // that gets skipped.
    return;
  }
  for (const RealmId retired : retired_realms_) {
    if (retired == realm) {
      return;  // retiring twice would hand one slot to two documents
    }
  }
  retired_realms_.push_back(realm);
  util::AddPerformanceCounter(util::PerfCounterId::JsRealmsRetired);
}

std::optional<RealmId> Interpreter::CreateRealm() {
  if (!retired_realms_.empty()) {
    // Oldest first, so a slot is reused as late as possible -- which is when the
    // objects the previous occupant left behind are most likely to have been
    // collected already.
    const RealmId reused = retired_realms_.front();
    retired_realms_.erase(retired_realms_.begin());
    const RealmId previous = current_realm_;
    Object* const previous_synced_from = realm_synced_from_;
    EnterRealm(reused);
    const bool populated = PopulateRealm(reused);
    EnterRealm(previous);
    realm_synced_from_ = previous_synced_from;
    if (!populated) {
      return std::nullopt;
    }
    return reused;
  }
  if (realms_.size() >= kMaxRealms) {
    // ADR 0042 §4. The count is page-controlled -- one more `<iframe>` is one
    // more realm -- so it is bounded, and refusing is reported by the frame not
    // running script rather than by tearing anything down.
    util::AddPerformanceCounter(util::PerfCounterId::JsRealmsRefused);
    return std::nullopt;
  }
  const auto id = static_cast<RealmId>(realms_.size());
  realms_.push_back(std::make_unique<Realm>());
  // Current *before* populating, so that every native `InstallGlobals` creates
  // records this realm rather than whichever one was running. That is the whole
  // reason `InstallGlobals` takes no realm parameter: the ambient answer is
  // correct at every one of its several hundred allocation sites, and a parameter
  // threaded through all of them is several hundred chances to pass the wrong one.
  const RealmId previous = current_realm_;
  Object* const previous_synced_from = realm_synced_from_;
  EnterRealm(id);
  const bool populated = PopulateRealm(id);
  if (!populated) {
    // Out of heap part way through. The realm stays in the list rather than being
    // popped: objects created before the failure already record `id`, and
    // shrinking the list under them would make `RealmIndex` name a realm that no
    // longer exists. A half-built realm is inert -- nothing will run in it,
    // because the caller is about to be told it has none.
    if (previous < realms_.size()) {
      EnterRealm(previous);
      realm_synced_from_ = previous_synced_from;
    }
    return std::nullopt;
  }
  if (id != kMainRealm) {
    EnterRealm(previous);
    realm_synced_from_ = previous_synced_from;
  }
  return id;
}

bool Interpreter::PopulateRealm(RealmId realm) {
  Realm& target = *realms_[realm];
  target.global = heap_.AllocateObject(Object::Kind::Plain);
  target.global_scope = heap_.AllocateEnvironment(nullptr);
  Intrinsics& into = target.intrinsics;
  into.object_prototype = heap_.AllocateObject(Object::Kind::Plain);
  into.array_prototype = heap_.AllocateObject(Object::Kind::Plain);
  into.function_prototype = heap_.AllocateObject(Object::Kind::Plain);
  into.string_prototype = heap_.AllocateObject(Object::Kind::Plain);
  into.regexp_prototype = heap_.AllocateObject(Object::Kind::Plain);
  into.promise_prototype = heap_.AllocateObject(Object::Kind::Plain);
  into.generator_prototype = heap_.AllocateObject(Object::Kind::Plain);
  into.async_generator_prototype = heap_.AllocateObject(Object::Kind::Plain);
  if (target.global == nullptr || target.global_scope == nullptr ||
      into.object_prototype == nullptr || into.array_prototype == nullptr ||
      into.function_prototype == nullptr || into.string_prototype == nullptr ||
      into.regexp_prototype == nullptr || into.promise_prototype == nullptr ||
      into.generator_prototype == nullptr || into.async_generator_prototype == nullptr) {
    return false;
  }
  InstallGlobals();
  util::AddPerformanceCounter(util::PerfCounterId::JsRealmsCreated);
  return true;
}

void Interpreter::EnterRealm(RealmId realm) {
  if (realm >= realms_.size()) {
    // An id this interpreter never handed out. Nothing can produce one except a
    // caller inventing it, and switching to a realm that does not exist would be
    // a null dereference on the next property access -- so it is a no-op, and the
    // running realm stays whatever it was.
    return;
  }
  if (current_realm_ != realm) {
    util::AddPerformanceCounter(util::PerfCounterId::JsRealmSwitches);
  }
  current_realm_ = realm;
  realm_ = realms_[realm].get();
  // So that everything allocated from here on records this realm. The heap is the
  // one place that cannot forget -- see AllocateObject.
  heap_.SetAllocationRealm(realm);
}

Object* Interpreter::GlobalOf(RealmId realm) {
  return realm < realms_.size() ? realms_[realm]->global : nullptr;
}

Environment* Interpreter::GlobalScopeOf(RealmId realm) {
  return realm < realms_.size() ? realms_[realm]->global_scope : nullptr;
}

Interpreter::RealmScope::RealmScope(Interpreter& owner, RealmId realm)
    : owner_(owner), previous_(owner.current_realm_), previous_host_(owner.host_realm_),
      previous_synced_from_(owner.realm_synced_from_) {
  owner_.EnterRealm(realm);
  // The host entering a realm is the only thing that decides which realm a
  // top-level program belongs to, so this is the one place `host_realm_` moves.
  // `EnterRealm` deliberately does not touch it: that one also runs from
  // `SyncRealm`, which follows the callee and must leave the program's realm
  // alone. Read back from `current_realm_` rather than from `realm`, so an id that
  // was never handed out -- which `EnterRealm` refuses -- does not become the
  // host's realm either.
  owner_.host_realm_ = owner_.current_realm_;
  // Cleared rather than set: the next callee the machine sees must not be
  // compared against a function from before this scope, or a call that happens to
  // re-enter the same function would skip the sync and run in the wrong realm.
  owner_.realm_synced_from_ = nullptr;
}

Interpreter::RealmScope::~RealmScope() {
  owner_.EnterRealm(previous_);
  owner_.host_realm_ = previous_host_;
  owner_.realm_synced_from_ = previous_synced_from_;
}

}  // namespace microbrowser::js
