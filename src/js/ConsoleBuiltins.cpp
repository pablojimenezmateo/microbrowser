// `console`: the one channel a page has to say something the browser will keep.
//
// Its own translation unit because Builtins.cpp is the *language* -- Math,
// JSON, the constructors -- and this is not part of the language at all. It is
// a diagnostic surface the host owns, with host state behind it (group depth,
// counters, timers) that nothing else in the engine has an opinion about. The
// module's line cap is what said so out loud.
//
// **Collected rather than printed, and that is a security property.** A page
// must not be able to write to the terminal the browser was started from; the
// host reads `ConsoleOutput()` and decides.
//
// The method list is longer than three because an *absent* console method is a
// TypeError that takes the rest of a script with it. youtube.com is the
// measurement: one `console.info(...)` in Polymer's legacy shim ended the
// kevlar bundle. `info`, `debug`, `trace`, `dir`, `dirxml` and `table` differ
// from `log` only in a severity this browser has nowhere to put, so aliasing
// them is honest rather than a stub -- nothing is missing behind the name.
// `group`, `count`, `time` and `assert` are *implemented*, for the same
// reason: a no-op that lies about counting is the thing ADR 0012 forbids.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"

namespace microbrowser::js {

void Interpreter::InstallConsole() {
  const auto install = [this](Object* target, const char* name, NativeFunction function) {
    InstallNative(target, name, std::move(function));
  };

  // --- console --------------------------------------------------------------
  // Collected rather than printed. A page must not be able to write to the
  // terminal the browser was started from, and a test needs to read what was
  // logged.
  Object* console = NewObject();
  // The console's own state -- how deep the groups are, what each `count`
  // label stands at, and when each `time` label started.
  //
  // Captured by the lambdas in a shared_ptr rather than held on the
  // interpreter or as properties of the object. Not on the interpreter because
  // none of it is language state; not as properties because a page could then
  // read and write the browser's bookkeeping, and `console.count` disagreeing
  // with itself is a small thing that would be entirely a page's doing.
  // Invisible to the collector is fine and only fine because there is not a
  // single `Value` in it.
  struct ConsoleState {
    int group_depth = 0;
    std::unordered_map<std::string, std::uint64_t> counts;
    std::unordered_map<std::string, double> timers;
  };
  const auto state = std::make_shared<ConsoleState>();

  // Joins arguments the way every console does, and indents by group depth.
  const auto compose = [state](const std::vector<Value>& arguments, std::size_t from) {
    std::string line(static_cast<std::size_t>(state->group_depth) * 2, ' ');
    for (std::size_t i = from; i < arguments.size(); ++i) {
      if (i != from) {
        line.push_back(' ');
      }
      // The pure conversion, deliberately: a console line must not be able to
      // run a page's `toString`. Logging is a thing the *host* does, often
      // while inspecting a value it does not trust, and a getter that runs
      // there would be a page choosing when the browser executes its code.
      line += ToString(arguments[i]);
    }
    return line;
  };
  const auto log = [this, compose](NativeCall& call) {
    console_.push_back(compose(call.arguments, 0));
    return Value::Undefined();
  };
  // The whole log-shaped family, and it is a family rather than three names
  // because the alternative is an absence -- and an absent `console.info` is a
  // TypeError that takes the rest of the script with it. youtube.com is where
  // that showed: one `console.info("LegacyDataMixin will be applied...")` in
  // Polymer's legacy shim ended the kevlar bundle. These differ from `log`
  // only in a severity this browser has nowhere to put, so aliasing them is
  // the honest implementation rather than a stub: nothing is missing behind
  // the name.
  for (const char* name : {"log", "warn", "error", "info", "debug", "trace", "dir", "dirxml",
                           "table", "group", "groupCollapsed"}) {
    install(console, name, log);
  }
  // `group` and `groupCollapsed` log their label *and* indent what follows.
  const auto group = [state, log](NativeCall& call) mutable {
    log(call);
    ++state->group_depth;
    return Value::Undefined();
  };
  install(console, "group", group);
  install(console, "groupCollapsed", group);
  install(console, "groupEnd", [state](NativeCall&) {
    if (state->group_depth > 0) {
      --state->group_depth;
    }
    return Value::Undefined();
  });
  install(console, "assert", [this, compose](NativeCall& call) {
    if (ToBoolean(Argument(call.arguments, 0))) {
      return Value::Undefined();
    }
    std::string line = "Assertion failed";
    const std::string rest = compose(call.arguments, 1);
    if (!rest.empty()) {
      line += ": " + rest;
    }
    console_.push_back(std::move(line));
    return Value::Undefined();
  });
  // A label defaults to "default" in all four of these, which is the spec's
  // own word and not a placeholder.
  const auto label = [](const std::vector<Value>& arguments) {
    const Value& given = Argument(arguments, 0);
    return given.IsUndefined() ? std::string("default") : ToString(given);
  };
  install(console, "count", [this, state, label](NativeCall& call) {
    const std::string name = label(call.arguments);
    const std::uint64_t at = ++state->counts[name];
    console_.push_back(name + ": " + std::to_string(at));
    return Value::Undefined();
  });
  install(console, "countReset", [state, label](NativeCall& call) {
    state->counts.erase(label(call.arguments));
    return Value::Undefined();
  });
  install(console, "time", [state, label](NativeCall& call) {
    state->timers[label(call.arguments)] = call.interpreter.NowMilliseconds();
    return Value::Undefined();
  });
  // Real elapsed time rather than a no-op, and at the same millisecond
  // resolution `Date.now()` is held to -- see the note at the top of
  // DateBuiltins.cpp. A finer clock here would be a timing probe wearing a
  // diagnostic's name.
  const auto elapsed = [this, state, label](NativeCall& call, bool ending) {
    const std::string name = label(call.arguments);
    const auto found = state->timers.find(name);
    if (found == state->timers.end()) {
      console_.push_back("Timer '" + name + "' does not exist");
      return Value::Undefined();
    }
    console_.push_back(name + ": " +
                       ToString(Value::Number(NowMilliseconds() - found->second)) + "ms");
    if (ending) {
      state->timers.erase(found);
    }
    return Value::Undefined();
  };
  install(console, "timeEnd", [elapsed](NativeCall& call) { return elapsed(call, true); });
  install(console, "timeLog", [elapsed](NativeCall& call) { return elapsed(call, false); });
  install(console, "clear", [this, state](NativeCall&) {
    console_.clear();
    state->group_depth = 0;
    return Value::Undefined();
  });
  global_scope_->Declare("console", Value::Obj(console), false);
}

}  // namespace microbrowser::js
