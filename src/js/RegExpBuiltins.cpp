#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"
#include "js/StringUnits.h"
#include "js/RegExp.h"
#include "js/RegExpSupport.h"

// RegExp.prototype, the RegExp constructor, and the String methods that take a
// pattern.
//
// One rule runs through all of it and is worth stating once: **a match is
// found against bytes and reported in bytes.** `index` counts bytes, and so do
// the offsets every method here slices with. That is the same unit
// `String.prototype.length` and `s[i]` already use, so the three agree; it is
// not the unit a browser uses, and the note on the RegExp class says what
// changing it would cost.

namespace microbrowser::js {

namespace {

// `lastIndex`, as the spec reads it: a non-negative integer, saturating rather
// than wrapping, because a page can assign anything to it.
//
// A page reads and writes it in code units and the matcher works in bytes, so
// the conversion happens here and in WriteLastIndex -- the two together are the
// only place the property crosses between the two measures.
std::size_t ReadLastIndex(const Object& object, const std::string& text) {
  const Value* value = object.Get("lastIndex");
  if (value == nullptr) {
    return 0;
  }
  const double number = ToNumber(*value);
  if (std::isnan(number) || number <= 0.0) {
    return 0;
  }
  const double limit = static_cast<double>(kMaxAllocationLength);
  return ByteOffsetOfUnit(text, static_cast<std::size_t>(number >= limit ? limit : number));
}

void WriteLastIndex(Object& object, const std::string& text, std::size_t at) {
  object.Set("lastIndex", Value::Number(static_cast<double>(UnitOffsetOfByte(text, at))));
}

// The array `exec` returns: the matched text, then one entry per group, with
// `index`, `input` and `groups` on the side. An absent group is undefined
// rather than empty, which is the distinction a page tests with `if (m[2])`.
Value MakeMatchResult(Interpreter& interpreter, const RegExp& pattern, const RegExpMatch& match,
                      const std::string& text) {
  std::vector<Value> groups;
  groups.reserve(match.GroupCount());
  for (std::size_t group = 0; group < match.GroupCount(); ++group) {
    groups.push_back(match.Participated(group)
                         ? Value::String(std::string(match.Group(text, group)))
                         : Value::Undefined());
  }
  const Value result = interpreter.NewArrayValue(std::move(groups));
  if (!result.IsObject()) {
    return result;  // the heap is full; the caller sees undefined
  }
  // In code units, which is what every other index in the language is.
  // The matcher works over bytes -- a pattern and a subject are both UTF-8 and
  // a byte match is a character match -- so this is the one place the two
  // measures meet, and it converts once here rather than at each caller.
  result.object->Set("index",
                     Value::Number(static_cast<double>(UnitOffsetOfByte(text, match.Begin()))));
  result.object->Set("input", Value::String(text));

  Value named = Value::Undefined();
  for (std::size_t group = 1; group < pattern.GroupNames().size(); ++group) {
    if (pattern.GroupNames()[group].empty()) {
      continue;
    }
    if (!named.IsObject()) {
      named = interpreter.NewObjectValue();
      if (!named.IsObject()) {
        break;
      }
    }
    named.object->Set(pattern.GroupNames()[group],
                      match.Participated(group)
                          ? Value::String(std::string(match.Group(text, group)))
                          : Value::Undefined());
  }
  result.object->Set("groups", named);
  return result;
}

// One step of a global match: the next match at or after `from`, and where the
// next step should start.
//
// The empty-match advance is the subtle part. A pattern that matches nothing
// -- `/a*/g` against "b" -- would otherwise return the same empty match at the
// same position forever, so the position moves on by one. Every global loop
// here goes through this, rather than each repeating the rule.
struct Step {
  std::optional<RegExpMatch> match;
  std::size_t next = 0;
};

Step NextMatch(const RegExp& pattern, const std::string& text, std::size_t from) {
  Step step;
  step.match = pattern.Exec(text, from, pattern.Flags().sticky);
  if (!step.match.has_value()) {
    return step;
  }
  step.next = step.match->End();
  if (step.match->End() == step.match->Begin()) {
    step.next = step.match->End() + 1;
  }
  return step;
}

// GetSubstitution, with the capture groups a pattern makes available. The
// string form is the one a page reaches for most often, and `$1` in it is the
// whole reason the byte-oriented offsets above have to be right.
std::string Substitute(std::string_view replacement, const RegExp& pattern,
                       const RegExpMatch& match, const std::string& text) {
  if (replacement.find('$') == std::string_view::npos) {
    return std::string(replacement);
  }
  const std::size_t begin = match.Begin();
  const std::size_t end = match.End();
  std::string out;
  out.reserve(replacement.size());
  for (std::size_t i = 0; i < replacement.size(); ++i) {
    if (replacement[i] != '$' || i + 1 == replacement.size()) {
      out.push_back(replacement[i]);
      continue;
    }
    const char next = replacement[i + 1];
    if (next == '$') {
      out.push_back('$');
      i += 1;
      continue;
    }
    if (next == '&') {
      out += text.substr(begin, end - begin);
      i += 1;
      continue;
    }
    if (next == '`') {
      out += text.substr(0, begin);
      i += 1;
      continue;
    }
    if (next == '\'') {
      out += text.substr(end);
      i += 1;
      continue;
    }
    if (next == '<') {
      const std::size_t close = replacement.find('>', i + 2);
      if (close == std::string_view::npos) {
        out.push_back('$');
        continue;  // no closing `>`: not a substitution at all
      }
      const std::string_view name = replacement.substr(i + 2, close - i - 2);
      const std::size_t group = pattern.GroupNamed(name);
      if (group != 0 && match.Participated(group)) {
        out += std::string(match.Group(text, group));
      }
      i = close;
      continue;
    }
    if (next >= '0' && next <= '9') {
      // Two digits win over one when the pattern has that many groups, which
      // is what makes `$12` mean group 12 rather than group 1 and a `2`.
      std::size_t group = static_cast<std::size_t>(next - '0');
      std::size_t consumed = 1;
      if (i + 2 < replacement.size() && replacement[i + 2] >= '0' &&
          replacement[i + 2] <= '9') {
        const std::size_t wider = group * 10 + static_cast<std::size_t>(replacement[i + 2] - '0');
        if (wider >= 1 && wider <= pattern.GroupCount()) {
          group = wider;
          consumed = 2;
        }
      }
      if (group >= 1 && group <= pattern.GroupCount()) {
        if (match.Participated(group)) {
          out += std::string(match.Group(text, group));
        }
        i += consumed;
        continue;
      }
    }
    out.push_back('$');  // not a form that means anything: left as written
  }
  return out;
}

// The replacement for one match, from either form the argument can take.
// Reports an abrupt completion from a callback through `call` rather than
// swallowing it.
bool ReplacementFor(NativeCall& call, const Value& replacement, const RegExp& pattern,
                    const RegExpMatch& match, const std::string& text, std::string& out) {
  if (!replacement.IsObject() || !replacement.object->IsCallable()) {
    out = Substitute(ToString(replacement), pattern, match, text);
    return true;
  }
  // (matched, ...groups, offset, string), which is the order a page's callback
  // destructures.
  std::vector<Value> arguments;
  arguments.reserve(match.GroupCount() + 2);
  for (std::size_t group = 0; group < match.GroupCount(); ++group) {
    arguments.push_back(match.Participated(group)
                            ? Value::String(std::string(match.Group(text, group)))
                            : Value::Undefined());
  }
  // In code units, like every other position a page sees.
  arguments.push_back(Value::Number(static_cast<double>(UnitOffsetOfByte(text, match.Begin()))));
  arguments.push_back(Value::String(text));
  const Result replaced =
      call.interpreter.CallFunction(replacement, Value::Undefined(), arguments);
  if (replaced.IsAbrupt()) {
    call.ThrowValue(replaced.value);
    return false;
  }
  out = ToString(replaced.value);
  return true;
}

// A pattern argument, however it was written.
//
// `'a.c'.match('.')` matches any character, because match converts a
// non-pattern argument into one. `'a.c'.replace('.', '-')` replaces the dot,
// because replace does not. The two rules are genuinely different and reading
// them off the spec is the only way to get them right, so the conversion is
// one function used by exactly the methods that perform it.
struct PatternArgument {
  RegExp pattern;
  // The RegExp object the pattern came from, when it came from one. Null for a
  // converted string, which has no `lastIndex` to write back to.
  Object* object = nullptr;
};

// Whether a page's own object answered instead. `out` is what it returned.
//
// Checked before anything is compiled: the object may not be a pattern at all
// and may not have a `source` to compile. The method is called with the
// subject string and whatever else the operation takes, which is the shape the
// spec gives every one of the five.
bool TryPatternProtocol(NativeCall& call, const char* which, const Value& pattern,
                        const std::vector<Value>& arguments, Value& out) {
  Object* method = call.interpreter.PatternProtocol(pattern, which);
  if (method == nullptr) {
    return false;
  }
  const Result answered = call.interpreter.CallFunction(Value::Obj(method), pattern, arguments);
  out = answered.IsAbrupt() ? call.ThrowValue(answered.value) : answered.value;
  return true;
}

bool ReadPatternArgument(NativeCall& call, const Value& value, bool force_global,
                         PatternArgument& out) {
  if (const RegExp* existing = call.interpreter.RegExpOf(value)) {
    out.pattern = *existing;
    out.object = value.object;
    return true;
  }
  RegExpFlags flags;
  flags.global = force_global;
  std::string error;
  out.pattern = RegExp::Compile(value.IsUndefined() ? std::string() : ToString(value), flags,
                                error);
  if (!out.pattern.IsValid()) {
    call.Throw("SyntaxError", "invalid regular expression: " + error);
    return false;
  }
  return true;
}

// The pattern and flags a RegExp constructor call was given, from either the
// two-string form or the copy-an-existing-one form.
bool ReadConstructorArguments(NativeCall& call, std::string& source, std::string& flags) {
  const Value pattern = Argument(call.arguments, 0);
  const Value given_flags = Argument(call.arguments, 1);
  if (const RegExp* existing = call.interpreter.RegExpOf(pattern)) {
    source = existing->Source();
    flags = given_flags.IsUndefined() ? existing->Flags().Text() : ToString(given_flags);
    return true;
  }
  source = pattern.IsUndefined() ? std::string() : ToString(pattern);
  flags = given_flags.IsUndefined() ? std::string() : ToString(given_flags);
  return true;
}

}  // namespace

// --- The String methods that take a pattern --------------------------------

Value RegExpSplit(NativeCall& call, const Value& pattern, const std::string& text,
                  const Value& limit) {
  const RegExp* expression = call.interpreter.RegExpOf(pattern);
  if (expression == nullptr) {
    return Value::Undefined();
  }
  std::size_t remaining = kMaxAllocationLength;
  if (!limit.IsUndefined()) {
    const double requested = ToNumber(limit);
    remaining = std::isnan(requested) || requested <= 0.0
                    ? 0
                    : static_cast<std::size_t>(std::min(requested,
                                                        static_cast<double>(remaining)));
  }

  std::vector<Value> pieces;
  if (remaining == 0) {
    return call.interpreter.NewArrayValue(std::move(pieces));
  }
  // Splitting the empty string is its own case in the spec: the result is one
  // empty piece unless the pattern matches, and no pieces if it does.
  if (text.empty()) {
    if (!expression->Exec(text, 0, false).has_value()) {
      pieces.push_back(Value::String(std::string()));
    }
    return call.interpreter.NewArrayValue(std::move(pieces));
  }

  // `piece_start` is where the piece being built began; `at` is where to look
  // for the next separator. A separator that ends exactly where the piece
  // began contributes nothing and only moves the search on -- that is what
  // stops `'ab'.split(/x*/)` from emitting an empty piece between every
  // character forever, and it is the one rule in the spec's loop that is not
  // obvious.
  std::size_t piece_start = 0;
  std::size_t at = 0;
  while (at < text.size()) {
    const std::optional<RegExpMatch> match = expression->Exec(text, at, false);
    if (!match.has_value() || match->Begin() >= text.size()) {
      break;
    }
    if (match->End() == piece_start) {
      at = match->Begin() + 1;
      continue;
    }
    pieces.push_back(Value::String(text.substr(piece_start, match->Begin() - piece_start)));
    if (pieces.size() >= remaining) {
      return call.interpreter.NewArrayValue(std::move(pieces));
    }
    // A separator's capture groups become part of the result, which is what
    // makes `'a1b'.split(/(\d)/)` three pieces rather than two.
    for (std::size_t group = 1; group < match->GroupCount(); ++group) {
      pieces.push_back(match->Participated(group)
                           ? Value::String(std::string(match->Group(text, group)))
                           : Value::Undefined());
      if (pieces.size() >= remaining) {
        return call.interpreter.NewArrayValue(std::move(pieces));
      }
    }
    piece_start = match->End();
    at = piece_start;
  }
  pieces.push_back(Value::String(text.substr(piece_start)));
  return call.interpreter.NewArrayValue(std::move(pieces));
}

Value RegExpReplace(NativeCall& call, const Value& pattern, const std::string& text,
                    const Value& replacement, bool all) {
  const RegExp* expression = call.interpreter.RegExpOf(pattern);
  if (expression == nullptr) {
    return Value::Undefined();
  }
  const bool every = all || expression->Flags().global;

  std::string out;
  std::size_t copied = 0;
  std::size_t at = 0;
  for (;;) {
    const Step step = NextMatch(*expression, text, at);
    if (!step.match.has_value()) {
      break;
    }
    std::string piece;
    if (!ReplacementFor(call, replacement, *expression, *step.match, text, piece)) {
      return Value::Undefined();  // the callback threw; `call` carries it
    }
    out += text.substr(copied, step.match->Begin() - copied);
    out += piece;
    copied = step.match->End();
    at = step.next;
    if (!every || at > text.size()) {
      break;
    }
    if (out.size() > kMaxAllocationLength) {
      return call.Throw("RangeError", "replacement result is too long");
    }
  }
  out += text.substr(copied);
  return Value::String(std::move(out));
}

// --- RegExp.prototype ------------------------------------------------------

void Interpreter::InstallRegExpPrototype() {
  const auto install = [this](Object* target, const char* name, NativeFunction function) {
    InstallNative(target, name, std::move(function));
  };

  // A getter reads the compiled pattern rather than a stored property, so a
  // page cannot make `source` disagree with what is actually matched.
  const auto accessor = [this](const char* name, NativeFunction function) {
    if (Object* getter = NewNative(name, std::move(function))) {
      intrinsics().regexp_prototype->DefineAccessor(name, getter, nullptr);
    }
  };
  const auto flag = [&accessor](const char* name, bool RegExpFlags::*member) {
    accessor(name, [member](NativeCall& call) {
      const RegExp* pattern = call.interpreter.RegExpOf(call.self);
      return pattern == nullptr ? Value::Undefined() : Value::Bool(pattern->Flags().*member);
    });
  };

  // `source` and `flags` are own data properties on the instance rather than
  // accessors here -- see NewRegExpValue for why. The booleans below are
  // derived from the compiled pattern, so they stay accessors and cannot be
  // made to disagree with it.
  flag("global", &RegExpFlags::global);
  flag("ignoreCase", &RegExpFlags::ignore_case);
  flag("multiline", &RegExpFlags::multiline);
  flag("dotAll", &RegExpFlags::dot_all);
  flag("sticky", &RegExpFlags::sticky);
  flag("unicode", &RegExpFlags::unicode);
  flag("hasIndices", &RegExpFlags::has_indices);

  install(intrinsics().regexp_prototype, "exec", [](NativeCall& call) {
    const RegExp* pattern = call.interpreter.RegExpOf(call.self);
    if (pattern == nullptr) {
      return call.Throw("TypeError", "RegExp.prototype.exec called on a non-RegExp");
    }
    const std::string text = ToString(Argument(call.arguments, 0));
    // `g` and `y` make a regex stateful, and the state is a property a page
    // reads and writes.
    const bool stateful = pattern->Flags().global || pattern->Flags().sticky;
    const std::size_t from = stateful ? ReadLastIndex(*call.self.object, text) : 0;
    const std::optional<RegExpMatch> match =
        from > text.size() ? std::nullopt : pattern->Exec(text, from, pattern->Flags().sticky);
    if (!match.has_value()) {
      if (stateful) {
        call.self.object->Set("lastIndex", Value::Number(0.0));
      }
      return Value::Null();
    }
    if (stateful) {
      WriteLastIndex(*call.self.object, text, match->End());
    }
    return MakeMatchResult(call.interpreter, *pattern, *match, text);
  });

  install(intrinsics().regexp_prototype, "test", [](NativeCall& call) {
    const RegExp* pattern = call.interpreter.RegExpOf(call.self);
    if (pattern == nullptr) {
      return call.Throw("TypeError", "RegExp.prototype.test called on a non-RegExp");
    }
    const std::string text = ToString(Argument(call.arguments, 0));
    const bool stateful = pattern->Flags().global || pattern->Flags().sticky;
    const std::size_t from = stateful ? ReadLastIndex(*call.self.object, text) : 0;
    const std::optional<RegExpMatch> match =
        from > text.size() ? std::nullopt : pattern->Exec(text, from, pattern->Flags().sticky);
    if (stateful) {
      call.self.object->Set("lastIndex",
                            Value::Number(match.has_value()
                                              ? static_cast<double>(
                                                    UnitOffsetOfByte(text, match->End()))
                                              : 0.0));
    }
    return Value::Bool(match.has_value());
  });

  install(intrinsics().regexp_prototype, "toString", [](NativeCall& call) {
    const RegExp* pattern = call.interpreter.RegExpOf(call.self);
    if (pattern == nullptr) {
      return Value::String(std::string("/(?:)/"));
    }
    return Value::String("/" + (pattern->Source().empty() ? "(?:)" : pattern->Source()) + "/" +
                         pattern->Flags().Text());
  });

  // --- The constructor ------------------------------------------------------

  Object* constructor = NewNative("RegExp", [](NativeCall& call) {
    std::string source;
    std::string flag_text;
    ReadConstructorArguments(call, source, flag_text);
    const std::optional<RegExpFlags> flags = RegExpFlags::Parse(flag_text);
    if (!flags.has_value()) {
      return call.Throw("SyntaxError", "invalid regular expression flags: " + flag_text);
    }
    std::string error;
    RegExp pattern = RegExp::Compile(source, *flags, error);
    if (!pattern.IsValid()) {
      return call.Throw("SyntaxError", "invalid regular expression: " + error);
    }
    return call.interpreter.NewRegExpValue(std::move(pattern));
  });
  if (constructor != nullptr) {
    constructor->Set("prototype", Value::Obj(intrinsics().regexp_prototype));
    intrinsics().regexp_prototype->SetHidden("constructor", Value::Obj(constructor));
    realm_->global_scope->Declare("RegExp", Value::Obj(constructor), false);
  }

  // --- The String methods that only exist for patterns ----------------------

  install(intrinsics().string_prototype, "match", [](NativeCall& call) {
    // The page's own object first: `Symbol.match` is how a library stands in
    // for a pattern, and asking after compiling would have compiled its
    // *string form* instead.
    Value answered;
    if (TryPatternProtocol(call, "match", Argument(call.arguments, 0), {call.self}, answered)) {
      return answered;
    }
    const std::string text = ToString(call.self);
    PatternArgument argument;
    if (!ReadPatternArgument(call, Argument(call.arguments, 0), false, argument)) {
      return Value::Undefined();  // the conversion threw; `call` carries it
    }
    const RegExp* expression = &argument.pattern;
    if (!expression->Flags().global) {
      const std::optional<RegExpMatch> match = expression->Exec(text, 0, false);
      return match.has_value()
                 ? MakeMatchResult(call.interpreter, *expression, *match, text)
                 : Value::Null();
    }
    // With `g` the answer is every matched string and no capture information,
    // which is a different shape from the non-global one and catches out
    // anyone who assumes otherwise.
    std::vector<Value> found;
    std::size_t at = 0;
    while (at <= text.size()) {
      const Step step = NextMatch(*expression, text, at);
      if (!step.match.has_value()) {
        break;
      }
      found.push_back(Value::String(std::string(step.match->Group(text, 0))));
      at = step.next;
      if (found.size() >= kMaxAllocationLength) {
        break;
      }
    }
    if (argument.object != nullptr) {
      // A global match leaves the regex rewound, which is what makes calling
      // `match` twice with the same literal give the same answer twice.
      argument.object->Set("lastIndex", Value::Number(0.0));
    }
    if (found.empty()) {
      return Value::Null();
    }
    return call.interpreter.NewArrayValue(std::move(found));
  });

  install(intrinsics().string_prototype, "matchAll", [](NativeCall& call) {
    // The page's own object first: `Symbol.matchAll` is how a library stands in
    // for a pattern, and asking after compiling would have compiled its
    // *string form* instead.
    Value answered;
    if (TryPatternProtocol(call, "matchAll", Argument(call.arguments, 0), {call.self}, answered)) {
      return answered;
    }
    const std::string text = ToString(call.self);
    const Value given = Argument(call.arguments, 0);
    // A pattern handed to matchAll must be global. The spec makes this a
    // TypeError rather than an implicit `g`, because the two readings of
    // `s.matchAll(/x/)` differ by an infinite loop.
    if (const RegExp* supplied = call.interpreter.RegExpOf(given)) {
      if (!supplied->Flags().global) {
        return call.Throw("TypeError", "matchAll requires a global regular expression");
      }
    }
    PatternArgument argument;
    if (!ReadPatternArgument(call, given, true, argument)) {
      return Value::Undefined();
    }

    // A real iterator, produced lazily. A page writes `for (const m of
    // s.matchAll(re))` over a large document and breaks out of it, and
    // building every match first would do all that work for nothing.
    Value iterator = call.interpreter.NewObjectValue();
    if (!iterator.IsObject()) {
      return iterator;
    }
    // The state goes in properties rather than in a capture, because a
    // capture is invisible to the collector -- the rule stated on NativeCall.
    // The pattern is stored as the RegExp object it came from, or as a fresh
    // one when the argument was a string, so the compiled program stays
    // reachable through the heap's table.
    iterator.object->SetHidden("#target", Value::String(text));
    iterator.object->SetHidden("#index", Value::Number(0.0));
    iterator.object->SetHidden("#pattern",
                         argument.object != nullptr
                             ? Value::Obj(argument.object)
                             : call.interpreter.NewRegExpValue(argument.pattern));

    const auto step = [](NativeCall& inner) {
      Value result = inner.interpreter.NewObjectValue();
      if (!result.IsObject() || !inner.self.IsObject()) {
        return result;
      }
      Object* state = inner.self.object;
      const Value* target = state->GetOwn("#target");
      const Value* at = state->GetOwn("#index");
      const Value* pattern_value = state->GetOwn("#pattern");
      const RegExp* pattern =
          pattern_value == nullptr ? nullptr : inner.interpreter.RegExpOf(*pattern_value);
      const std::string text_of = target == nullptr ? std::string() : target->AsString();
      const std::size_t from = at == nullptr ? 0 : static_cast<std::size_t>(ToNumber(*at));
      const Step found =
          pattern == nullptr || from > text_of.size() ? Step{} : NextMatch(*pattern, text_of, from);
      if (!found.match.has_value()) {
        result.object->Set("value", Value::Undefined());
        result.object->Set("done", Value::Bool(true));
        return result;
      }
      state->SetHidden("#index", Value::Number(static_cast<double>(found.next)));
      result.object->Set(
          "value", MakeMatchResult(inner.interpreter, *pattern, *found.match, text_of));
      result.object->Set("done", Value::Bool(false));
      return result;
    };
    iterator.object->Set("next", call.interpreter.NewNativeValue("next", step));
    // An iterator is itself iterable, which is what lets its result be spread
    // or fed straight to `for...of`.
    iterator.object->Set(
        PropertyKey::Symbol(call.interpreter.SymbolIterator()),
        call.interpreter.NewNativeValue("[Symbol.iterator]",
                                        [](NativeCall& inner) { return inner.self; }));
    return iterator;
  });

  install(intrinsics().string_prototype, "search", [](NativeCall& call) {
    // The page's own object first: `Symbol.search` is how a library stands in
    // for a pattern, and asking after compiling would have compiled its
    // *string form* instead.
    Value answered;
    if (TryPatternProtocol(call, "search", Argument(call.arguments, 0), {call.self}, answered)) {
      return answered;
    }
    const std::string text = ToString(call.self);
    PatternArgument argument;
    if (!ReadPatternArgument(call, Argument(call.arguments, 0), false, argument)) {
      return Value::Undefined();
    }
    const std::optional<RegExpMatch> match = argument.pattern.Exec(text, 0, false);
    return Value::Number(
        match.has_value() ? static_cast<double>(UnitOffsetOfByte(text, match->Begin())) : -1.0);
  });
}

}  // namespace microbrowser::js
