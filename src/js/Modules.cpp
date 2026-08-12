#include <string>
#include <utility>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"
#include "util/Env.h"

// Modules: what a specifier resolves to, what a module publishes, and the order
// they run in.
//
// **Loading is the host's job and linking is this file's.** A specifier is a
// string whose meaning is a URL question -- relative to what, over which
// protocol, past which privacy verdict -- and none of that belongs in a
// language engine. So the engine takes a resolver from the host: given a
// specifier and the module that asked for it, hand back a resolved name and the
// source. Everything after that is here.
//
// The order is the spec's, and it is the part worth getting right:
//
//   1. Load, depth first. A module's requests are read off its parse tree
//      before anything runs, so the whole graph is known before any of it
//      evaluates -- which is what makes a missing dependency an error at the
//      import rather than half way through.
//   2. Link, which here is binding names. Every import is resolved against the
//      exporting module's namespace.
//   3. Evaluate, in post-order. A module's dependencies have run before it
//      does, which is what makes a top-level `import` usable at the top level.
//
// A cycle is broken at load: a module already being loaded is not loaded again,
// and the one that closed the cycle sees its exports as they are when it runs.
//
// **One deviation, and it is written here rather than discovered.** An import
// is bound *by value* at link time, not as a live view of the exporting
// module's binding. So a module that exports a `let` and reassigns it later
// does not show the new value through an import, and a cyclic import of a
// binding whose module has not finished sees `undefined` rather than a
// ReferenceError. Live bindings need every use of an imported name to compile
// to an indirection, which is a scope analysis this engine does not have yet;
// binding by value is what every bundler's output already behaves like, and it
// is right for every module that exports functions and constants -- which is
// almost all of them.

namespace microbrowser::js {

namespace {

// How deep a graph may go before it is refused. A cycle is broken by the status
// check rather than by this, so reaching it means a genuinely deep chain --
// and the recursion below is C++ recursion, which is what has to be bounded.
constexpr int kMaxModuleDepth = 64;

}  // namespace

std::vector<std::string> ModuleImportSpecifiers(std::string_view source) {
  // Parse-only, and deliberately not "evaluate and see what it asks for": the
  // host has to fetch a module's whole static graph *before* anything in it runs,
  // because the resolver it will be asked through is synchronous and cannot go to
  // the network. This is what makes that possible.
  //
  // An unparseable module answers with nothing rather than with an error: the
  // parse error is the *evaluation's* to report, at the point where a page can
  // see it, and reporting it twice from two places is two different messages for
  // one fault.
  std::vector<std::string> specifiers;
  const ParseResult parsed = Parse(source);
  if (!parsed.Ok() || parsed.program == nullptr) {
    return specifiers;
  }
  for (const NodePtr& statement : parsed.program->children) {
    if (statement == nullptr) {
      continue;
    }
    // `import ... from "x"` and `export ... from "x"` both name a module, and a
    // graph that followed only the first would leave a re-export unfetched.
    const bool imports = statement->kind == NodeKind::ImportDeclaration;
    const bool re_exports =
        statement->kind == NodeKind::ExportDeclaration && !statement->string.empty();
    if ((imports || re_exports) && !statement->string.empty()) {
      specifiers.push_back(statement->string);
    }
  }
  return specifiers;
}

void Interpreter::SetModuleResolver(ModuleResolver resolver) {
  module_resolver_ = std::move(resolver);
}

void Interpreter::SetDynamicImportStarter(DynamicImportStarter starter) {
  dynamic_import_starter_ = std::move(starter);
}

Interpreter::Module* Interpreter::FindModule(const std::string& specifier) {
  const auto found = modules_.find(specifier);
  return found == modules_.end() ? nullptr : found->second.get();
}

Result Interpreter::LoadModule(const std::string& specifier, const std::string& referrer,
                               int depth, Module*& out) {
  out = nullptr;
  if (depth > kMaxModuleDepth) {
    return Throw("RangeError", "module graph is too deep");
  }
  if (!module_resolver_) {
    return Throw("TypeError", "modules are not available in this context");
  }
  std::string resolved;
  std::string source;
  if (!module_resolver_(specifier, referrer, resolved, source)) {
    return Throw("TypeError", "cannot resolve module '" + specifier + "'");
  }
  // Keyed by the *resolved* name, so two specifiers naming one file are one
  // module -- which is what makes a shared dependency shared rather than run
  // twice.
  if (Module* existing = FindModule(resolved)) {
    out = existing;
    // A module still loading is one that closed a cycle. Handing it back
    // half-built is what breaks the cycle; its exports are whatever they are
    // when the importer runs.
    return Result::Normal();
  }

  auto record = std::make_unique<Module>();
  record->specifier = resolved;
  ParseResult parsed = Parse(source);
  if (!parsed.Ok()) {
    const std::string message = parsed.errors.empty() ? "invalid module"
                                                      : parsed.errors.front().message;
    return Throw("SyntaxError", "in module '" + resolved + "': " + message);
  }
  // Held for the life of the interpreter, like every other program: a function
  // object points at its own body in the tree.
  programs_.push_back(std::move(parsed.program));
  record->program = programs_.back().get();
  record->source_length = source.size();
  record->scope = heap_.AllocateEnvironment(realm_->global_scope);
  record->exports = NewObject();
  if (record->scope == nullptr || record->exports == nullptr) {
    return Throw("RangeError", "out of memory");
  }
  // The one place `this` is undefined at the top level. A script gets the
  // global object (see Builtins.cpp); a module gets nothing, and declaring it
  // here rather than leaving it unbound is what shadows the global one -- a
  // module's scope has the global scope as its parent, so an absent binding
  // would find it.
  record->scope->Declare("this", Value::Undefined(), true);
  // A namespace object has no prototype: `ns.toString` must be the module's
  // export of that name or undefined, never Object.prototype's method.
  record->exports->SetPrototype(nullptr);
  if (shared_.symbol_to_string_tag != nullptr) {
    record->exports->SetHidden(PropertyKey::Symbol(shared_.symbol_to_string_tag),
                               Value::String(std::string("Module")));
  }
  Module* record_ptr = record.get();
  modules_.emplace(resolved, std::move(record));
  out = record_ptr;

  // Every specifier the module names, read off the tree before anything runs.
  for (const NodePtr& statement : record_ptr->program->children) {
    if (statement == nullptr) {
      continue;
    }
    const bool imports = statement->kind == NodeKind::ImportDeclaration;
    const bool re_exports =
        statement->kind == NodeKind::ExportDeclaration && !statement->string.empty();
    if ((imports || re_exports) && !statement->string.empty()) {
      Module* dependency = nullptr;
      const Result loaded = LoadModule(statement->string, resolved, depth + 1, dependency);
      if (loaded.IsAbrupt()) {
        record_ptr->status = Module::Status::Failed;
        return loaded;
      }
      record_ptr->requests.emplace_back(statement->string, dependency);
    }
  }
  return Result::Normal();
}

Result Interpreter::EvaluateModule(Module& module) {
  if (module.status == Module::Status::Evaluated) {
    return Result::Normal();
  }
  if (module.status == Module::Status::Failed) {
    return Result{Completion::Throw, module.error, {}};
  }
  if (module.status == Module::Status::Evaluating) {
    // The cycle. Its exports are whatever they are so far, which is the
    // language's answer too -- what differs here is that an unset one reads
    // as undefined rather than throwing.
    return Result::Normal();
  }
  module.status = Module::Status::Evaluating;

  // Dependencies first, in the order they were written. That is what makes a
  // top-level `import` usable at the top level of the importer.
  for (const auto& request : module.requests) {
    if (request.second == nullptr) {
      continue;
    }
    const Result ran = EvaluateModule(*request.second);
    if (ran.IsAbrupt()) {
      module.status = Module::Status::Failed;
      module.error = ran.value;
      return ran;
    }
  }

  // Link: bring every imported name into the module's own scope. By value --
  // see the note at the top of this file.
  const ScopeGuard guard(*this, module.scope);
  // `import.meta`, which is a fresh object per module and is the module's own
  // record as a page can see it. Bound under a name no source can write, so
  // both engines read it as an ordinary name and neither needs to know which
  // module it is running.
  if (Object* meta = NewObject()) {
    meta->Set("url", Value::String(module.specifier));
    module.scope->Declare("*meta*", Value::Obj(meta), true);
  }
  // `import(spec)`. A promise, because the language says so -- and here it is
  // an already-settled one, because loading is synchronous in this engine. A
  // page cannot tell the difference except in ordering, and the ordering is
  // the same one every `then` sees.
  //
  // The specifier resolves against *this* module, which is why the hook is per
  // module rather than a global: `import('./x.js')` means something different
  // depending on which file wrote it.
  const std::string referrer = module.specifier;
  if (Object* hook = NewNative("import", [referrer](NativeCall& call) {
        std::string specifier;
        const Result converted =
            call.interpreter.ToStringOf(Argument(call.arguments, 0), specifier);
        if (converted.IsAbrupt()) {
          return call.ThrowValue(converted.value);
        }
        return call.interpreter.ImportDynamically(specifier, referrer);
      })) {
    module.scope->Declare("*import*", Value::Obj(hook), true);
  }
  for (const NodePtr& statement : module.program->children) {
    if (statement == nullptr || statement->kind != NodeKind::ImportDeclaration) {
      continue;
    }
    Module* source = nullptr;
    for (const auto& request : module.requests) {
      if (request.first == statement->string) {
        source = request.second;
      }
    }
    if (source == nullptr) {
      continue;  // `import "side-effects"` names nothing
    }
    for (const NodePtr& entry : statement->children) {
      if (entry == nullptr || entry->kind != NodeKind::Import) {
        continue;
      }
      const auto kind = static_cast<std::uint8_t>(entry->number);
      if (kind == kImportNamespace) {
        module.scope->Declare(entry->string, Value::Obj(source->exports), true);
        continue;
      }
      const std::string exported =
          kind == kImportDefault
              ? std::string("default")
              : (entry->Child(0) == nullptr ? entry->string : entry->Child(0)->string);
      const Value* value = source->exports->GetOwn(exported);
      module.scope->Declare(entry->string, value == nullptr ? Value::Undefined() : *value,
                            true);
    }
  }

  // Run the body, on the machine, in the module's own scope.
  //
  // The machine rather than the tree-walker, and that is not a speed choice:
  // a module whose body was walked would create functions with AST bodies, and
  // an `async function` or a generator among them would be *refused* at the
  // call. A module is where a page's real code lives; it gets the real engine.
  static const bool tree_walk = util::EnvFlagEnabled("MICROBROWSER_JS_TREEWALK");
  bool ran_compiled = false;
  Result outcome = Result::Normal();
  if (!tree_walk) {
    if (std::unique_ptr<CompiledFunction> compiled =
            Compile(*module.program, module.source_length)) {
      vm_.programs.push_back(std::move(compiled));
      outcome = RunCompiled(*vm_.programs.back(), module.scope);
      ran_compiled = true;
    }
  }
  if (!ran_compiled) {
    HoistDeclarations(*module.program, *module.scope);
    HoistVars(*module.program, *module.scope);
    for (const NodePtr& statement : module.program->children) {
      if (statement == nullptr) {
        continue;
      }
      outcome = EvaluateStatement(*statement, *module.scope);
      if (outcome.IsAbrupt()) {
        break;
      }
    }
  }
  if (outcome.IsAbrupt()) {
    module.status = Module::Status::Failed;
    module.error = outcome.value;
    return outcome;
  }

  // Publish. Read out of the module's scope rather than written as the body
  // ran, because only this knows what a module *is* -- the compiler compiles
  // an export's declaration and nothing else.
  for (const NodePtr& statement : module.program->children) {
    if (statement == nullptr || statement->kind != NodeKind::ExportDeclaration) {
      continue;
    }
    const Result published = PublishExports(*statement, module);
    if (published.IsAbrupt()) {
      module.status = Module::Status::Failed;
      module.error = published.value;
      return published;
    }
  }

  module.status = Module::Status::Evaluated;
  // The namespace is frozen once the module has run: a page must not be able
  // to add an export that the module does not have, because another module may
  // already have imported the absence of it.
  module.exports->Restrict(Object::Integrity::Sealed);
  return Result::Normal();
}

Value Interpreter::ImportDynamically(const std::string& specifier,
                                     const std::string& referrer) {
  const Value promise = NewPromiseValue();
  if (!promise.IsObject()) {
    return Value::Undefined();
  }
  if (dynamic_import_starter_) {
    // The host fetches and settles this on a later turn. Rooted meanwhile: the
    // page's own `.then` chain holds the promise, but between this call and the
    // host's answer the *host* is the only other reference and it holds a raw
    // pointer -- and a raw pointer is not something the collector walks.
    PendingImports()->PushElement(promise);
    if (dynamic_import_starter_(specifier, referrer, promise.object)) {
      return promise;
    }
    // The host refused to start it. Rejected here rather than left pending,
    // because a promise nobody will settle is a page that waits forever.
    DropPendingImport(promise.object);
    SettleAsyncResult(promise.object,
                      MakeError("TypeError", "cannot resolve module '" + specifier + "'"), true);
    return promise;
  }
  Module* loaded = nullptr;
  const Result found = LoadModule(specifier, referrer, 1, loaded);
  if (found.IsAbrupt() || loaded == nullptr) {
    // A failed import rejects rather than throws: the caller is holding a
    // promise, and a throw here would be a synchronous failure from a call the
    // language says is asynchronous.
    SettleAsyncResult(promise.object, found.value, true);
    return promise;
  }
  const Result ran = EvaluateModule(*loaded);
  if (ran.IsAbrupt()) {
    SettleAsyncResult(promise.object, ran.value, true);
    return promise;
  }
  SettleAsyncResult(promise.object, Value::Obj(loaded->exports), false);
  return promise;
}

void Interpreter::SettleDynamicImport(Object* promise, std::string_view specifier,
                                     std::string_view referrer) {
  if (promise == nullptr) {
    return;
  }
  DropPendingImport(promise);
  Module* loaded = nullptr;
  const Result found = LoadModule(std::string(specifier), std::string(referrer), 1, loaded);
  if (found.IsAbrupt() || loaded == nullptr) {
    SettleAsyncResult(promise, found.value, true);
    return;
  }
  const Result ran = EvaluateModule(*loaded);
  if (ran.IsAbrupt()) {
    SettleAsyncResult(promise, ran.value, true);
    return;
  }
  SettleAsyncResult(promise, Value::Obj(loaded->exports), false);
}

// The promises handed out for imports nobody has answered yet.
//
// A JavaScript array on the global rather than a C++ container, because the
// collector cannot see a `js::Value` in a C++ field -- and the host's copy is a
// raw `Object*`, which is worse than invisible: it survives the collection that
// freed what it points at.
Object* Interpreter::PendingImports() {
  if (const Value* existing = realm_->global->GetOwn("#pending-imports")) {
    if (existing->IsObject()) {
      return existing->object;
    }
  }
  const Value list = NewArrayValue({});
  if (list.IsObject()) {
    realm_->global->SetHidden("#pending-imports", list);
  }
  return list.IsObject() ? list.object : nullptr;
}

void Interpreter::DropPendingImport(Object* promise) {
  Object* pending = PendingImports();
  if (pending == nullptr) {
    return;
  }
  std::vector<Value> kept;
  for (std::size_t i = 0; i < pending->ElementCount(); ++i) {
    const Value entry = pending->GetElement(i);
    if (!entry.IsObject() || entry.object != promise) {
      kept.push_back(entry);
    }
  }
  pending->SetElements(kept, std::vector<bool>(kept.size(), true));
}

Result Interpreter::PublishExports(const Node& statement, Module& module) {
  const auto flags = static_cast<std::uint8_t>(statement.number);

  if ((flags & kExportAll) != 0) {
    // `export * from "m"`, and `export * as ns from "m"`.
    Module* source = nullptr;
    for (const auto& request : module.requests) {
      if (request.first == statement.string) {
        source = request.second;
      }
    }
    if (source == nullptr) {
      return Result::Normal();
    }
    if (!statement.children.empty() && statement.Child(0) != nullptr &&
        statement.Child(0)->kind == NodeKind::Export) {
      module.exports->Set(statement.Child(0)->string, Value::Obj(source->exports));
      return Result::Normal();
    }
    for (const std::string& name : source->exports->Keys()) {
      // `default` is not re-exported by a star, which is the one name the form
      // leaves out -- and leaving it in would give two modules one default.
      if (name == "default") {
        continue;
      }
      const Value* value = source->exports->GetOwn(name);
      module.exports->Set(name, value == nullptr ? Value::Undefined() : *value);
    }
    return Result::Normal();
  }

  if ((flags & kExportDefault) != 0) {
    // The body bound it under a name no source can write. Reading it back is
    // what publishes it, and it is one lookup rather than a second evaluation
    // of the expression -- which would run its side effects twice.
    Value* bound = module.scope->Lookup("*default*");
    module.exports->Set("default", bound == nullptr ? Value::Undefined() : *bound);
    return Result::Normal();
  }

  if ((flags & kExportDeclaration) != 0) {
    const Node* declaration = statement.Child(0);
    if (declaration == nullptr) {
      return Result::Normal();
    }
    // Every name the declaration bound is published under its own name.
    std::vector<std::string> names;
    CollectDeclaredNames(*declaration, names);
    for (const std::string& name : names) {
      Value* bound = module.scope->Lookup(name);
      module.exports->Set(name, bound == nullptr ? Value::Undefined() : *bound);
    }
    return Result::Normal();
  }

  // `export { a, b as c }`, with or without a `from`.
  Module* source = nullptr;
  if (!statement.string.empty()) {
    for (const auto& request : module.requests) {
      if (request.first == statement.string) {
        source = request.second;
      }
    }
  }
  for (const NodePtr& entry : statement.children) {
    if (entry == nullptr || entry->kind != NodeKind::Export) {
      continue;
    }
    const std::string exported =
        entry->Child(0) == nullptr ? entry->string : entry->Child(0)->string;
    if (source != nullptr) {
      const Value* value = source->exports->GetOwn(entry->string);
      module.exports->Set(exported, value == nullptr ? Value::Undefined() : *value);
      continue;
    }
    Value* bound = module.scope->Lookup(entry->string);
    module.exports->Set(exported, bound == nullptr ? Value::Undefined() : *bound);
  }
  return Result::Normal();
}

void Interpreter::CollectDeclaredNames(const Node& node, std::vector<std::string>& out) {
  switch (node.kind) {
    case NodeKind::FunctionDeclaration:
    case NodeKind::ClassDeclaration:
      if (!node.string.empty()) {
        out.push_back(node.string);
      }
      return;
    case NodeKind::VariableDeclaration:
      for (const NodePtr& declarator : node.children) {
        if (declarator != nullptr && declarator->Child(0) != nullptr) {
          CollectDeclaredNames(*declarator->Child(0), out);
        }
      }
      return;
    case NodeKind::Identifier:
      out.push_back(node.string);
      return;
    // A destructuring export -- `export const {a, b} = o` -- publishes every
    // name the pattern binds, which is the same walk a binding does.
    case NodeKind::ArrayLiteral:
      for (const NodePtr& element : node.children) {
        if (element != nullptr) {
          CollectDeclaredNames(*element, out);
        }
      }
      return;
    case NodeKind::ObjectLiteral:
      for (const NodePtr& property : node.children) {
        if (property != nullptr && property->Child(0) != nullptr) {
          CollectDeclaredNames(*property->Child(0), out);
        }
      }
      return;
    case NodeKind::AssignmentPattern:
    case NodeKind::RestElement:
    case NodeKind::Spread:
      if (node.Child(0) != nullptr) {
        CollectDeclaredNames(*node.Child(0), out);
      }
      return;
    default:
      return;
  }
}

Result Interpreter::RunModule(std::string_view source, std::string_view specifier) {
  // The entry module is registered by hand rather than fetched: the host has
  // its source already, which is the whole difference between it and every
  // module it names.
  const std::string name(specifier);
  if (Module* existing = FindModule(name)) {
    return EvaluateModule(*existing);
  }
  ParseResult parsed = Parse(source);
  if (!parsed.Ok()) {
    const std::string message =
        parsed.errors.empty() ? "invalid module" : parsed.errors.front().message;
    return Throw("SyntaxError", message);
  }
  programs_.push_back(std::move(parsed.program));

  auto record = std::make_unique<Module>();
  record->specifier = name;
  record->program = programs_.back().get();
  record->source_length = source.size();
  record->scope = heap_.AllocateEnvironment(realm_->global_scope);
  record->exports = NewObject();
  if (record->scope == nullptr || record->exports == nullptr) {
    return Throw("RangeError", "out of memory");
  }
  // The one place `this` is undefined at the top level. A script gets the
  // global object (see Builtins.cpp); a module gets nothing, and declaring it
  // here rather than leaving it unbound is what shadows the global one -- a
  // module's scope has the global scope as its parent, so an absent binding
  // would find it.
  record->scope->Declare("this", Value::Undefined(), true);
  record->exports->SetPrototype(nullptr);
  Module* entry = record.get();
  modules_.emplace(name, std::move(record));

  for (const NodePtr& statement : entry->program->children) {
    if (statement == nullptr || statement->string.empty()) {
      continue;
    }
    if (statement->kind != NodeKind::ImportDeclaration &&
        statement->kind != NodeKind::ExportDeclaration) {
      continue;
    }
    Module* dependency = nullptr;
    const Result loaded = LoadModule(statement->string, name, 1, dependency);
    if (loaded.IsAbrupt()) {
      entry->status = Module::Status::Failed;
      return loaded;
    }
    entry->requests.emplace_back(statement->string, dependency);
  }

  const Result ran = EvaluateModule(*entry);
  if (ran.IsAbrupt()) {
    return ran;
  }
  // A module's completion value is not a thing the language defines -- there
  // is no console to print one to. The namespace is what a host wants back.
  DrainMicrotasks();
  return Result::Normal(Value::Obj(entry->exports));
}

}  // namespace microbrowser::js
