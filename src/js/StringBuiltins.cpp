#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"
#include "js/RegExpSupport.h"
#include "util/StringUtil.h"

// String.prototype.
//
// One deviation from the spec runs through all of it, and it is worth stating
// once rather than at every method: **a string here is a sequence of bytes, not
// of UTF-16 code units.** `length` already counts bytes and `s[i]` already
// yields one, both from before any of this existed, so `charAt`, `charCodeAt`,
// `at`, every index and every length below follow that rather than contradict
// it. For ASCII -- which is all of HTML's syntax, every CSS keyword, and most
// of what a page's script indexes into -- the two agree exactly. For anything
// above U+007F they do not: "é".length is 2 here and 1 in a browser.
//
// The fix is to store strings as UTF-16 and convert at the DOM boundary, which
// is a change to Value and to every consumer of it rather than to this file.
// Until then, being consistently byte-oriented is worth more than being
// UTF-16-correct in the four methods added last and byte-oriented in the two
// that came first: `s.charCodeAt(i)` and `s[i]` agree, and `fromCharCode`
// inverts `charCodeAt` exactly.
//
// `split`, `replace` and `replaceAll` each accept a pattern as well as a
// string. The pattern branch is in RegExpBuiltins.cpp and reached through
// RegExpSupport.h: what the two forms share is the method name and nothing
// else, and capture-group substitution does not belong in the file whose
// defining property is that it does no matching.

namespace microbrowser::js {

namespace {

// The spec's ToIntegerOrInfinity: truncate toward zero, and NaN is zero.
double ToInteger(double value) {
  return std::isnan(value) ? 0.0 : std::trunc(value);
}

// slice-style clamping: a negative index counts back from the end. Saturating
// rather than wrapping, so `s.slice(-1e300)` is 0 and `s.slice(1e300)` is the
// length instead of both being undefined behaviour in the cast.
std::size_t ClampRelative(double index, std::size_t size) {
  const double limit = static_cast<double>(size);
  const double at = ToInteger(index);
  return static_cast<std::size_t>(at < 0.0 ? std::max(limit + at, 0.0) : std::min(at, limit));
}

// substring-style clamping: a negative index is zero rather than an offset from
// the end. The two rules differ, which is the whole difference between
// `slice(-2)` and `substring(-2)`.
std::size_t ClampAbsolute(double index, std::size_t size) {
  return static_cast<std::size_t>(
      std::clamp(ToInteger(index), 0.0, static_cast<double>(size)));
}

bool IsJsWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// The receiver, as text. `this` is the string the method was read from, but a
// method detached and called on something else -- `String.prototype.trim.call(7)`
// -- gets that value converted rather than a crash.
std::string Self(const NativeCall& call) {
  return ToString(call.self);
}

// The substring `search` occupies in `text`, if the caller passed a pattern
// that can match at all.
std::size_t FindFrom(const std::string& text, const std::string& search, std::size_t from) {
  return from > text.size() ? std::string::npos : text.find(search, from);
}

// `$`-substitution in a replacement string, per GetSubstitution. `$1`..`$9`
// name capture groups, which need a regex engine, so they are left alone rather
// than replaced with an empty string -- an untouched `$1` is a visible bug
// where a silently empty one is not.
std::string Substitute(std::string_view replacement, const std::string& text,
                       std::size_t position, std::size_t matched_length) {
  if (replacement.find('$') == std::string_view::npos) {
    return std::string(replacement);
  }
  std::string out;
  out.reserve(replacement.size());
  for (std::size_t i = 0; i < replacement.size(); ++i) {
    if (replacement[i] != '$' || i + 1 == replacement.size()) {
      out.push_back(replacement[i]);
      continue;
    }
    switch (replacement[i + 1]) {
      case '$': out.push_back('$'); break;
      case '&': out += text.substr(position, matched_length); break;
      case '`': out += text.substr(0, position); break;
      case '\'': out += text.substr(position + matched_length); break;
      default: out.push_back('$'); continue;  // not a recognised form: literal
    }
    ++i;
  }
  return out;
}

// The replacement for one match: a function is called with the spec's
// (matched, position, string) and its result stringified; anything else is a
// string with `$`-substitution applied. Reports an abrupt completion from the
// callback through `call` rather than swallowing it.
bool ReplacementFor(NativeCall& call, const Value& replacement, const std::string& text,
                    std::size_t position, std::size_t matched_length, std::string& out) {
  if (!replacement.IsObject() || !replacement.object->IsCallable()) {
    out = Substitute(ToString(replacement), text, position, matched_length);
    return true;
  }
  const Result replaced = call.interpreter.CallFunction(
      replacement, Value::Undefined(),
      {Value::String(text.substr(position, matched_length)),
       Value::Number(static_cast<double>(position)), Value::String(text)});
  if (replaced.IsAbrupt()) {
    call.ThrowValue(replaced.value);
    return false;
  }
  out = ToString(replaced.value);
  return true;
}

}  // namespace

void Interpreter::InstallStringPrototype(Object* string_constructor) {
  const auto method = [this](const char* name, NativeFunction function) {
    InstallNative(string_prototype_, name, std::move(function));
  };

  // --- Identity -------------------------------------------------------------
  const NativeFunction identity = [](NativeCall& call) { return Value::String(Self(call)); };
  method("toString", identity);
  method("valueOf", identity);

  // --- Characters -----------------------------------------------------------
  method("charAt", [](NativeCall& call) {
    const std::string text = Self(call);
    const double at = ToInteger(ToNumber(Argument(call.arguments, 0)));
    // Out of range is the empty string, not undefined -- charAt and at differ
    // on exactly this.
    if (at < 0.0 || at >= static_cast<double>(text.size())) {
      return Value::String(std::string());
    }
    return Value::String(std::string(1, text[static_cast<std::size_t>(at)]));
  });
  method("at", [](NativeCall& call) {
    const std::string text = Self(call);
    double at = ToInteger(ToNumber(Argument(call.arguments, 0)));
    if (at < 0.0) {
      at += static_cast<double>(text.size());
    }
    if (at < 0.0 || at >= static_cast<double>(text.size())) {
      return Value::Undefined();
    }
    return Value::String(std::string(1, text[static_cast<std::size_t>(at)]));
  });
  method("charCodeAt", [](NativeCall& call) {
    const std::string text = Self(call);
    const double at = ToInteger(ToNumber(Argument(call.arguments, 0)));
    if (at < 0.0 || at >= static_cast<double>(text.size())) {
      return Value::Number(std::nan(""));  // out of range is NaN, not 0
    }
    return Value::Number(
        static_cast<double>(static_cast<unsigned char>(text[static_cast<std::size_t>(at)])));
  });

  // --- Searching ------------------------------------------------------------
  method("indexOf", [](NativeCall& call) {
    const std::string text = Self(call);
    const std::string search = ToString(Argument(call.arguments, 0));
    const std::size_t from = ClampAbsolute(ToNumber(Argument(call.arguments, 1)), text.size());
    const std::size_t found = FindFrom(text, search, from);
    return Value::Number(found == std::string::npos ? -1.0 : static_cast<double>(found));
  });
  method("lastIndexOf", [](NativeCall& call) {
    const std::string text = Self(call);
    const std::string search = ToString(Argument(call.arguments, 0));
    // An omitted or NaN position means "search the whole string", which is the
    // one place NaN is not treated as zero.
    const Value position = Argument(call.arguments, 1);
    const double numeric = ToNumber(position);
    const std::size_t from = position.IsUndefined() || std::isnan(numeric)
                                 ? text.size()
                                 : ClampAbsolute(numeric, text.size());
    const std::size_t found = text.rfind(search, from);
    return Value::Number(found == std::string::npos ? -1.0 : static_cast<double>(found));
  });
  method("includes", [](NativeCall& call) {
    const std::string text = Self(call);
    const std::string search = ToString(Argument(call.arguments, 0));
    const std::size_t from = ClampAbsolute(ToNumber(Argument(call.arguments, 1)), text.size());
    return Value::Bool(FindFrom(text, search, from) != std::string::npos);
  });
  method("startsWith", [](NativeCall& call) {
    const std::string text = Self(call);
    const std::string search = ToString(Argument(call.arguments, 0));
    const std::size_t from = ClampAbsolute(ToNumber(Argument(call.arguments, 1)), text.size());
    return Value::Bool(
        util::StartsWith(std::string_view(text).substr(from), search));
  });
  method("endsWith", [](NativeCall& call) {
    const std::string text = Self(call);
    const std::string search = ToString(Argument(call.arguments, 0));
    // The second argument is where the string is treated as ending, not where
    // to start looking -- `"abcd".endsWith("bc", 3)` is true.
    const Value end_position = Argument(call.arguments, 1);
    const std::size_t end = end_position.IsUndefined()
                                ? text.size()
                                : ClampAbsolute(ToNumber(end_position), text.size());
    return Value::Bool(util::EndsWith(std::string_view(text).substr(0, end), search));
  });

  // --- Slicing --------------------------------------------------------------
  method("slice", [](NativeCall& call) {
    const std::string text = Self(call);
    const std::size_t begin = ClampRelative(ToNumber(Argument(call.arguments, 0)), text.size());
    const Value end_value = Argument(call.arguments, 1);
    const std::size_t end = end_value.IsUndefined()
                                ? text.size()
                                : ClampRelative(ToNumber(end_value), text.size());
    // A reversed range is empty rather than an error. substring swaps instead.
    return Value::String(end <= begin ? std::string() : text.substr(begin, end - begin));
  });
  method("substring", [](NativeCall& call) {
    const std::string text = Self(call);
    std::size_t begin = ClampAbsolute(ToNumber(Argument(call.arguments, 0)), text.size());
    const Value end_value = Argument(call.arguments, 1);
    std::size_t end = end_value.IsUndefined()
                          ? text.size()
                          : ClampAbsolute(ToNumber(end_value), text.size());
    if (begin > end) {
      std::swap(begin, end);
    }
    return Value::String(text.substr(begin, end - begin));
  });
  method("concat", [](NativeCall& call) {
    std::string text = Self(call);
    for (const Value& argument : call.arguments) {
      const std::string part = ToString(argument);
      if (text.size() + part.size() > kMaxAllocationLength) {
        return call.Throw("RangeError", "string is too long");
      }
      text += part;
    }
    return Value::String(std::move(text));
  });

  // --- Case and whitespace --------------------------------------------------
  // ASCII-only, like every other case fold in this repository. Unicode case
  // conversion is table-driven and locale-sensitive (Turkish dotless i is the
  // standard example), and guessing at it is worse than not doing it.
  const auto fold = [](char from_first, char from_last, char to_first) {
    return [from_first, from_last, to_first](NativeCall& call) {
      std::string text = Self(call);
      for (char& c : text) {
        if (c >= from_first && c <= from_last) {
          c = static_cast<char>(c - from_first + to_first);
        }
      }
      return Value::String(std::move(text));
    };
  };
  method("toUpperCase", fold('a', 'z', 'A'));
  method("toLowerCase", fold('A', 'Z', 'a'));

  const auto trim = [](bool start, bool end) {
    return [start, end](NativeCall& call) {
      const std::string text = Self(call);
      std::size_t begin = 0;
      std::size_t stop = text.size();
      while (start && begin < stop && IsJsWhitespace(text[begin])) {
        ++begin;
      }
      while (end && stop > begin && IsJsWhitespace(text[stop - 1])) {
        --stop;
      }
      return Value::String(text.substr(begin, stop - begin));
    };
  };
  method("trim", trim(true, true));
  method("trimStart", trim(true, false));
  method("trimEnd", trim(false, true));

  // --- Building -------------------------------------------------------------
  method("repeat", [](NativeCall& call) {
    const std::string text = Self(call);
    const double count = ToInteger(ToNumber(Argument(call.arguments, 0)));
    if (count < 0.0 || std::isinf(count)) {
      return call.Throw("RangeError", "repeat count must be finite and non-negative");
    }
    // The multiplication is on a number a page chose. Checking against the
    // limit in doubles, before it is narrowed, is what keeps it from wrapping.
    if (count * static_cast<double>(text.size()) > static_cast<double>(kMaxAllocationLength)) {
      return call.Throw("RangeError", "string is too long");
    }
    std::string out;
    out.reserve(text.size() * static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
      out += text;
    }
    return Value::String(std::move(out));
  });

  const auto pad = [](bool at_start) {
    return [at_start](NativeCall& call) {
      const std::string text = Self(call);
      const double target = ToInteger(ToNumber(Argument(call.arguments, 0)));
      if (target > static_cast<double>(kMaxAllocationLength)) {
        return call.Throw("RangeError", "string is too long");
      }
      if (target <= static_cast<double>(text.size())) {
        return Value::String(text);  // already long enough: unchanged
      }
      const Value pad_value = Argument(call.arguments, 1);
      const std::string filler = pad_value.IsUndefined() ? " " : ToString(pad_value);
      if (filler.empty()) {
        return Value::String(text);  // an empty pad cannot fill anything
      }
      std::string filling;
      const std::size_t needed = static_cast<std::size_t>(target) - text.size();
      filling.reserve(needed);
      while (filling.size() < needed) {
        filling += filler;
      }
      filling.resize(needed);  // the last repeat is truncated, not dropped
      return Value::String(at_start ? filling + text : text + filling);
    };
  };
  method("padStart", pad(true));
  method("padEnd", pad(false));

  // --- Splitting and replacing ---------------------------------------------
  method("split", [](NativeCall& call) {
    const std::string text = Self(call);
    const Value limit_value = Argument(call.arguments, 1);
    const std::size_t limit =
        limit_value.IsUndefined()
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(ToUint32(ToNumber(limit_value)));
    std::vector<Value> parts;
    const Value separator_value = Argument(call.arguments, 0);
    if (call.interpreter.RegExpOf(separator_value) != nullptr) {
      return RegExpSplit(call, separator_value, text, limit_value);
    }
    if (limit == 0) {
      return call.interpreter.NewArrayValue(std::move(parts));
    }
    if (separator_value.IsUndefined()) {
      // No separator at all is one part, whatever the string contains. This is
      // not the same as an empty separator, which is every character.
      parts.push_back(Value::String(text));
      return call.interpreter.NewArrayValue(std::move(parts));
    }
    const std::string separator = ToString(separator_value);
    if (separator.empty()) {
      for (std::size_t i = 0; i < text.size() && parts.size() < limit; ++i) {
        parts.push_back(Value::String(std::string(1, text[i])));
      }
      return call.interpreter.NewArrayValue(std::move(parts));
    }
    std::size_t at = 0;
    while (parts.size() < limit) {
      const std::size_t found = text.find(separator, at);
      if (found == std::string::npos) {
        // The tail after the last separator is a part even when it is empty,
        // which is why `"a,".split(",")` has two entries.
        parts.push_back(Value::String(text.substr(at)));
        break;
      }
      parts.push_back(Value::String(text.substr(at, found - at)));
      at = found + separator.size();
    }
    return call.interpreter.NewArrayValue(std::move(parts));
  });

  const auto replace = [](bool all) {
    return [all](NativeCall& call) {
      const std::string text = Self(call);
      const Value pattern = Argument(call.arguments, 0);
      const Value replacement = Argument(call.arguments, 1);
      if (call.interpreter.RegExpOf(pattern) != nullptr) {
        return RegExpReplace(call, pattern, text, replacement, all);
      }
      const std::string search = ToString(pattern);
      std::string out;
      std::size_t at = 0;
      do {
        const std::size_t found = FindFrom(text, search, at);
        if (found == std::string::npos) {
          break;
        }
        std::string replaced;
        if (!ReplacementFor(call, replacement, text, found, search.size(), replaced)) {
          return Value::Undefined();  // the callback threw; `call` carries it
        }
        if (out.size() + (found - at) + replaced.size() > kMaxAllocationLength) {
          return call.Throw("RangeError", "string is too long");
        }
        out += text.substr(at, found - at);
        out += replaced;
        // An empty pattern matches at every position including the end, so the
        // cursor has to move even though the match consumed nothing -- without
        // this, `"ab".replaceAll("", "-")` never terminates.
        at = found + std::max<std::size_t>(search.size(), 1);
        if (search.empty() && found < text.size()) {
          out.push_back(text[found]);
        }
      } while (all && at <= text.size());
      if (at <= text.size()) {
        out += text.substr(at);
      }
      return Value::String(std::move(out));
    };
  };
  method("replace", replace(false));
  method("replaceAll", replace(true));

  // --- The constructor's own properties -------------------------------------
  string_constructor->Set("prototype", Value::Obj(string_prototype_));
  string_prototype_->Set("constructor", Value::Obj(string_constructor));
  InstallNative(string_constructor, "fromCharCode", [](NativeCall& call) {
    // The exact inverse of charCodeAt under this file's byte model: one byte
    // per argument, masked the way the spec masks to sixteen bits.
    std::string out;
    if (call.arguments.size() > kMaxAllocationLength) {
      return call.Throw("RangeError", "string is too long");
    }
    out.reserve(call.arguments.size());
    for (const Value& argument : call.arguments) {
      out.push_back(static_cast<char>(ToUint32(ToNumber(argument)) & 0xFFu));
    }
    return Value::String(std::move(out));
  });
}

}  // namespace microbrowser::js
