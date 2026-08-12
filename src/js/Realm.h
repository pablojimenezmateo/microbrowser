#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace microbrowser::js {

class Object;
class Environment;

// Which realm, as an index into the interpreter's list rather than a pointer.
//
// An index because it is stored on every callable object (`Object::RealmIndex`)
// and has to survive the list growing when a page appends an `<iframe>`; a
// pointer into a `std::vector` does not. Sixteen bits because that fits in
// padding `Object` already had, so a realm costs nothing per object -- see
// ADR 0042 §2.
//
// Zero is always the realm the interpreter was constructed with, which is the
// page's own. That is relied on: an object allocated before any realm switch
// has ever happened is in realm 0 by having its field default-initialised.
using RealmId = std::uint16_t;
inline constexpr RealmId kMainRealm = 0;

// How many realms one interpreter will hand out. ADR 0042 §4: a page creates a
// realm by appending an `<iframe>`, so the count is page-controlled, and every
// page-controlled count here is bounded. Each realm costs a full set of
// intrinsics -- on the order of a hundred objects plus a global scope holding
// every builtin -- so an unbounded count is a memory-exhaustion vector reachable
// from three lines of script.
//
// Not chosen against what the index can hold (65,535) but against what a real
// page does: a document with sixty-four scripted same-origin frames is already
// outside everything in ADR 0007.
inline constexpr std::size_t kMaxRealms = 64;

// One realm's intrinsics: the prototypes a page can reach and compare.
//
// Per-realm exactly because they are comparable. `frames[0].Array === Array`
// answering *false* is the observable that tells a page it is looking at a
// second global, and it is what every library uses to decide whether a value
// came from somewhere else. The well-known symbols are deliberately *not* here
// -- they live on the interpreter and are shared, because a protocol that
// crosses a realm has to find the same cell or it does not connect at all. See
// ADR 0042 §1.
//
// This keeps `WellKnown`'s shape -- fields plus one `Roots()` next to them --
// for the reason that struct was written that way: every field is a GC root, and
// a root list maintained apart from the fields is a use-after-free waiting for a
// page to allocate enough.
struct Intrinsics {
  Object* object_prototype = nullptr;
  Object* array_prototype = nullptr;
  Object* function_prototype = nullptr;
  // Where a string's methods live, so that `"a".trim` and
  // `String.prototype.trim` are the same function object. A string is a
  // primitive here rather than a boxed object, so GetProperty consults this
  // directly instead of walking a chain from a wrapper.
  Object* string_prototype = nullptr;
  Object* regexp_prototype = nullptr;
  Object* promise_prototype = nullptr;
  // Where a number's methods live. A number is a primitive here, like a
  // string, so GetProperty consults this directly rather than boxing.
  Object* number_prototype = nullptr;
  // And a boolean's, on exactly the same terms. Two methods live on it and
  // both matter more than their size suggests: `true.toString()` is what
  // ToPrimitive reaches for, so without this a boolean in a string context
  // is a TypeError rather than "true".
  Object* boolean_prototype = nullptr;
  // Where a bigint's methods live. A bigint is a primitive here, like a
  // number, so GetProperty consults this directly rather than boxing.
  Object* bigint_prototype = nullptr;
  // The two the typed arrays need to find again: a typed array made without
  // a buffer allocates one and has to give it the right prototype, and the
  // nine constructors share one prototype between them.
  Object* array_buffer_prototype = nullptr;
  Object* typed_array_prototype = nullptr;
  // Where `next`, `throw` and `return` live, and the `Symbol.iterator` that
  // returns the generator itself. One object shared by every generator
  // rather than one per generator function -- so `Object.getPrototypeOf(g())`
  // is this rather than `gen.prototype`, which is the one place a page could
  // tell and is not a place any page looks.
  Object* generator_prototype = nullptr;
  // The same for an async generator, and a separate object rather than the
  // one above because every method on it differs: `next` hands back a
  // promise of `{value, done}` rather than the pair itself, and the hook it
  // answers to is `Symbol.asyncIterator`.
  Object* async_generator_prototype = nullptr;

  std::vector<Object*> Roots() const {
    return {object_prototype,       array_prototype,     function_prototype,
            string_prototype,       regexp_prototype,    promise_prototype,
            number_prototype,       boolean_prototype,   bigint_prototype,
            array_buffer_prototype, typed_array_prototype, generator_prototype,
            async_generator_prototype};
  }
};

// The other half of the same decision: the cells that are *not* per realm.
//
// Here rather than on `Interpreter` because "what is shared" is only meaningful
// beside "what is not", and the two used to be one struct -- splitting them and
// leaving half in another header is how the line between them stops being
// readable. One member on the interpreter, one list next to the fields, for the
// reason `Intrinsics` keeps that shape: every field is a GC root, and a root list
// maintained apart from the fields is a use-after-free waiting for a page to
// allocate enough.
//
// Two categories earn their place here, and ADR 0042 §1 has the argument:
//
// - **The well-known symbols.** The specification shares these across realms and
//   every protocol that crosses one depends on it -- an array from one realm
//   iterated by a `for...of` compiled in another has to find the *same*
//   `Symbol.iterator` cell, or the protocol does not connect. A per-realm one
//   would make spreading a cross-realm array silently produce nothing.
// - **The two internal signals.** A page cannot reach either: neither is ever a
//   property of anything nameable, and every path that could return one converts
//   it first. They are compared by identity by the engine and by nobody else, so
//   a second copy would be a cost with no observable attached.
struct SharedCells {
  // The cell every iteration goes through. Held here rather than looked up
  // through the global `Symbol`, which a page can reassign -- the protocol has to
  // keep working when it does.
  Object* symbol_iterator = nullptr;
  // What `for await` resolves against, held for the reason above.
  Object* symbol_async_iterator = nullptr;
  // The three hooks an *operator* consults, held for the reason the two above
  // are: `+`, `instanceof` and `Object.prototype.toString` have to find them
  // whatever a page did to the global `Symbol`.
  Object* symbol_to_primitive = nullptr;
  Object* symbol_has_instance = nullptr;
  Object* symbol_to_string_tag = nullptr;
  // The prototype of the value a forced return travels as.
  //
  // Making a generator return means running every `finally` between the `yield`
  // it stopped at and the end of its body, and the only run-time path that does
  // that is the one a throw takes. So a forced return *is* a throw, of a value
  // nothing else can produce -- this is the marker that says so, and the unwinder
  // reads it to know that no `catch` may see it.
  Object* return_signal = nullptr;
  // What a short-circuited optional chain travels as, in the tree-walker.
  //
  // `a?.b.c` with a nullish `a` is undefined for the whole expression, so the
  // innermost link has to tell the ones outside it to give up too -- and a
  // tree-walker's links are C++ frames, which can only say so with a value. One
  // shared object rather than one per short-circuit, compared by identity; the
  // parser marks where the chain ends, and that mark is where it turns back into
  // undefined.
  Object* chain_signal = nullptr;

  // Every well-known symbol cell by its name on `Symbol`, including the five
  // above -- those are cached pointers into this, because they are read on hot
  // paths and a map lookup per iteration step is not free.
  //
  // It exists because a second realm re-runs `InstallGlobals`, and the cells must
  // not be re-created: `frames[0].Symbol.iterator === Symbol.iterator` is
  // specified to be true, and more than that, `PatternProtocol` resolves `Symbol`
  // out of whichever global scope is running -- so two cells would mean a RegExp
  // from one realm answering a protocol query in another and finding nothing.
  // Realm 0 allocates them; every later realm installs the same cells onto its
  // own `Symbol` constructor.
  std::unordered_map<std::string, Object*> symbols;

  std::vector<Object*> Roots() const {
    std::vector<Object*> roots{return_signal, chain_signal};
    roots.reserve(symbols.size() + 2);
    for (const auto& [name, cell] : symbols) {
      roots.push_back(cell);
    }
    return roots;
  }
};

// One global object, the scope its declarations live in, and its intrinsics.
//
// ADR 0042. A browsing context is a realm: the page's own document is realm 0,
// and each same-origin `<iframe>` that runs script is another one in the *same*
// interpreter and therefore the same heap -- because same-origin frames hand
// each other live objects, and an object from one heap in another is a
// use-after-free waiting for the first collection. A cross-origin frame gets its
// own `Interpreter` instead, which is why there is nothing in this struct about
// origins: by the time a realm exists, that decision has been made.
struct Realm {
  Object* global = nullptr;
  Environment* global_scope = nullptr;
  Intrinsics intrinsics;

  std::vector<Object*> Roots() const {
    std::vector<Object*> roots = intrinsics.Roots();
    roots.push_back(global);
    return roots;
  }
};

}  // namespace microbrowser::js
