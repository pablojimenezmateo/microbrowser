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
#include "js/StringUnits.h"
#include "util/StringUtil.h"

// String.prototype.
//
// One thing runs through all of it and is worth stating once rather than at
// every method: **an index here is a UTF-16 code unit, and the storage is
// UTF-8.** The language defines a string as a sequence of code units and every
// index a method takes or returns is measured in them; this engine stores
// UTF-8 because that is what the network, the HTML parser, the CSS parser and
// the DOM all speak.
//
// The conversion between the two lives in StringUnits.h, in one place, so that
// `charCodeAt`, `slice` and a regular expression's match index cannot disagree
// about what position 3 means. Every method below that takes or returns an
// index goes through it. For an ASCII string -- which is nearly all of them --
// the conversion is the identity and costs one word-at-a-time scan.
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
    InstallNative(well_known_.string_prototype, name, std::move(function));
  };

  // --- Identity -------------------------------------------------------------
  const NativeFunction identity = [](NativeCall& call) { return Value::String(Self(call)); };
  method("toString", identity);
  method("valueOf", identity);

  // --- Characters -----------------------------------------------------------
  method("charAt", [](NativeCall& call) {
    const std::string text = Self(call);
    const double at = ToInteger(ToNumber(Argument(call.arguments, 0)));
    const auto length = static_cast<double>(Utf16Length(text));
    // Out of range is the empty string, not undefined -- charAt and at differ
    // on exactly this.
    if (at < 0.0 || at >= length) {
      return Value::String(std::string());
    }
    const auto unit = static_cast<std::size_t>(at);
    return Value::String(SubstringUnits(text, unit, unit + 1));
  });
  method("at", [](NativeCall& call) {
    const std::string text = Self(call);
    const auto length = static_cast<double>(Utf16Length(text));
    double at = ToInteger(ToNumber(Argument(call.arguments, 0)));
    if (at < 0.0) {
      at += length;
    }
    if (at < 0.0 || at >= length) {
      return Value::Undefined();
    }
    const auto unit = static_cast<std::size_t>(at);
    return Value::String(SubstringUnits(text, unit, unit + 1));
  });
  method("charCodeAt", [](NativeCall& call) {
    const std::string text = Self(call);
    const double at = ToInteger(ToNumber(Argument(call.arguments, 0)));
    if (at < 0.0 || at >= static_cast<double>(Utf16Length(text))) {
      return Value::Number(std::nan(""));  // out of range is NaN, not 0
    }
    return Value::Number(CodeUnitAt(text, static_cast<std::size_t>(at)));
  });

  // --- Searching ------------------------------------------------------------
  // The searches take a start position in code units and answer one in code
  // units; the search itself is over bytes, because a byte match and a code
  // unit match are the same match -- UTF-8 has no false positives, a sequence
  // cannot begin inside another one.
  method("indexOf", [](NativeCall& call) {
    const std::string text = Self(call);
    const std::string search = ToString(Argument(call.arguments, 0));
    const std::size_t from = ByteOffsetOfUnit(
        text, ClampAbsolute(ToNumber(Argument(call.arguments, 1)), Utf16Length(text)));
    const std::size_t found = FindFrom(text, search, from);
    return Value::Number(found == std::string::npos
                             ? -1.0
                             : static_cast<double>(UnitOffsetOfByte(text, found)));
  });
  method("lastIndexOf", [](NativeCall& call) {
    const std::string text = Self(call);
    const std::string search = ToString(Argument(call.arguments, 0));
    // An omitted or NaN position means "search the whole string", which is the
    // one place NaN is not treated as zero.
    const Value position = Argument(call.arguments, 1);
    const double numeric = ToNumber(position);
    const std::size_t from =
        position.IsUndefined() || std::isnan(numeric)
            ? text.size()
            : ByteOffsetOfUnit(text, ClampAbsolute(numeric, Utf16Length(text)));
    const std::size_t found = text.rfind(search, from);
    return Value::Number(found == std::string::npos
                             ? -1.0
                             : static_cast<double>(UnitOffsetOfByte(text, found)));
  });
  method("includes", [](NativeCall& call) {
    const std::string text = Self(call);
    const std::string search = ToString(Argument(call.arguments, 0));
    const std::size_t from = ByteOffsetOfUnit(
        text, ClampAbsolute(ToNumber(Argument(call.arguments, 1)), Utf16Length(text)));
    return Value::Bool(FindFrom(text, search, from) != std::string::npos);
  });
  method("startsWith", [](NativeCall& call) {
    const std::string text = Self(call);
    const std::string search = ToString(Argument(call.arguments, 0));
    const std::size_t from = ByteOffsetOfUnit(
        text, ClampAbsolute(ToNumber(Argument(call.arguments, 1)), Utf16Length(text)));
    return Value::Bool(util::StartsWith(std::string_view(text).substr(from), search));
  });
  method("endsWith", [](NativeCall& call) {
    const std::string text = Self(call);
    const std::string search = ToString(Argument(call.arguments, 0));
    // The second argument is where the string is treated as ending, not where
    // to start looking -- `"abcd".endsWith("bc", 3)` is true.
    const Value end_position = Argument(call.arguments, 1);
    const std::size_t end =
        end_position.IsUndefined()
            ? text.size()
            : ByteOffsetOfUnit(text,
                               ClampAbsolute(ToNumber(end_position), Utf16Length(text)));
    return Value::Bool(util::EndsWith(std::string_view(text).substr(0, end), search));
  });

  // --- Slicing --------------------------------------------------------------
  method("slice", [](NativeCall& call) {
    const std::string text = Self(call);
    const std::size_t length = Utf16Length(text);
    const std::size_t begin = ClampRelative(ToNumber(Argument(call.arguments, 0)), length);
    const Value end_value = Argument(call.arguments, 1);
    const std::size_t end = end_value.IsUndefined()
                                ? length
                                : ClampRelative(ToNumber(end_value), length);
    // A reversed range is empty rather than an error. substring swaps instead.
    return Value::String(end <= begin ? std::string() : SubstringUnits(text, begin, end));
  });
  method("substring", [](NativeCall& call) {
    const std::string text = Self(call);
    const std::size_t length = Utf16Length(text);
    std::size_t begin = ClampAbsolute(ToNumber(Argument(call.arguments, 0)), length);
    const Value end_value = Argument(call.arguments, 1);
    std::size_t end = end_value.IsUndefined()
                          ? length
                          : ClampAbsolute(ToNumber(end_value), length);
    if (begin > end) {
      std::swap(begin, end);
    }
    return Value::String(SubstringUnits(text, begin, end));
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
  // Past ASCII, and deliberately not all the way: the mapping in StringUnits
  // covers the ranges where case is arithmetic -- Latin-1, Latin Extended,
  // Greek, Cyrillic -- and leaves the rest alone. Full coverage is a megabyte
  // of tables, and the locale-sensitive cases (Turkish dotless i is the
  // standard example) need a locale this engine does not have. Leaving a
  // character as itself is the answer that cannot be wrong.
  method("toUpperCase",
         [](NativeCall& call) { return Value::String(ToUpper(Self(call))); });
  method("toLowerCase",
         [](NativeCall& call) { return Value::String(ToLower(Self(call))); });

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
    // In bytes rather than code units, because bytes are what is allocated.
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
      // The target is a length in *code units*, so a string of accented
      // characters pads to the width a page asked for rather than to a
      // shorter one its byte count happened to reach.
      const std::size_t units = Utf16Length(text);
      if (target <= static_cast<double>(units)) {
        return Value::String(text);  // already long enough: unchanged
      }
      const Value pad_value = Argument(call.arguments, 1);
      const std::string filler = pad_value.IsUndefined() ? " " : ToString(pad_value);
      if (filler.empty()) {
        return Value::String(text);  // an empty pad cannot fill anything
      }
      std::string filling;
      const std::size_t needed = static_cast<std::size_t>(target) - units;
      while (Utf16Length(filling) < needed) {
        filling += filler;
      }
      // The last repeat is truncated, not dropped -- and truncated in code
      // units, which is what keeps it from cutting a character in half.
      filling = SubstringUnits(filling, 0, needed);
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
      // Every code *unit*, not every byte and not every character: `[...s]`
      // splits by code point and `s.split('')` does not, and a page that uses
      // one where it meant the other is relying on the difference.
      const std::size_t units = Utf16Length(text);
      for (std::size_t i = 0; i < units && parts.size() < limit; ++i) {
        parts.push_back(Value::String(SubstringUnits(text, i, i + 1)));
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

  // --- The rest of the surface ----------------------------------------------

  method("codePointAt", [](NativeCall& call) {
    // The whole code point at an index, where charCodeAt gives one unit of it.
    // The two differ only on a surrogate pair, which is the case this exists
    // for: an index at the start of one sees the whole character.
    const std::string text = Self(call);
    const double index = ToInteger(ToNumber(Argument(call.arguments, 0)));
    if (index < 0.0 || index >= static_cast<double>(Utf16Length(text))) {
      return Value::Undefined();
    }
    return Value::Number(CodePointAt(text, static_cast<std::size_t>(index)));
  });
  // No Unicode tables here, so the canonical forms cannot be computed. Handing
  // the string back unchanged is right for every ASCII string, which is the
  // overwhelming majority, and honest for the rest -- inventing a
  // normalisation is worse than not having one.
  method("normalize", [](NativeCall& call) {
    const Value form = Argument(call.arguments, 0);
    if (!form.IsUndefined()) {
      const std::string name = ToString(form);
      if (name != "NFC" && name != "NFD" && name != "NFKC" && name != "NFKD") {
        return call.Throw("RangeError", "invalid normalization form");
      }
    }
    return Value::String(Self(call));
  });
  // Byte order, which is code point order for UTF-8 and therefore the same
  // answer a locale-unaware comparison gives. A real collation needs locale
  // data this engine does not have; a page sorting a list gets a stable,
  // predictable order rather than a fabricated one.
  method("localeCompare", [](NativeCall& call) {
    const std::string self = Self(call);
    const std::string other = ToString(Argument(call.arguments, 0));
    return Value::Number(self < other ? -1.0 : (self > other ? 1.0 : 0.0));
  });
  method("substr", [](NativeCall& call) {
    // Legacy, and still in use. Unlike `slice` the second argument is a
    // *length*, and a negative start counts from the end.
    const std::string text = Self(call);
    const std::size_t units = Utf16Length(text);
    const std::size_t start = ClampRelative(ToNumber(Argument(call.arguments, 0)), units);
    const Value length_value = Argument(call.arguments, 1);
    double length = length_value.IsUndefined()
                        ? static_cast<double>(units - start)
                        : ToInteger(ToNumber(length_value));
    length = std::min(std::max(length, 0.0), static_cast<double>(units - start));
    return Value::String(SubstringUnits(text, start, start + static_cast<std::size_t>(length)));
  });

  // The locale-aware spellings, which without locale data are the plain ones.
  // Installed as separate function objects rather than aliases so that a page
  // replacing one does not silently replace the other.
  for (const char* pair : {"toLocaleUpperCase", "toLocaleLowerCase", "trimLeft", "trimRight"}) {
    const std::string name(pair);
    const std::string canonical = name == "toLocaleUpperCase"  ? "toUpperCase"
                                  : name == "toLocaleLowerCase" ? "toLowerCase"
                                  : name == "trimLeft"          ? "trimStart"
                                                                : "trimEnd";
    InstallNative(well_known_.string_prototype, pair,
                  [canonical](NativeCall& call) {
                    const Value method_value =
                        call.interpreter.GetPropertyValue(call.self, canonical);
                    const Result out =
                        call.interpreter.CallFunction(method_value, call.self, call.arguments);
                    return out.IsAbrupt() ? call.ThrowValue(out.value) : out.value;
                  });
  }

  // --- The constructor's own properties -------------------------------------
  string_constructor->Set("prototype", Value::Obj(well_known_.string_prototype));
  well_known_.string_prototype->Set("constructor", Value::Obj(string_constructor));
  InstallNative(string_constructor, "fromCharCode", [](NativeCall& call) {
    // The exact inverse of charCodeAt: one *code unit* per argument, masked to
    // sixteen bits the way the spec masks. A high surrogate is held so that a
    // low one following it joins into the character they name together --
    // which is what makes `fromCharCode(...[...s].map(c => c.charCodeAt(0)))`
    // round-trip an emoji rather than break it in half.
    std::string out;
    if (call.arguments.size() > kMaxAllocationLength) {
      return call.Throw("RangeError", "string is too long");
    }
    std::uint32_t pending = 0;
    for (const Value& argument : call.arguments) {
      AppendCodeUnit(out, static_cast<std::uint16_t>(ToUint32(ToNumber(argument)) & 0xFFFFu),
                     pending);
    }
    FlushCodeUnit(out, pending);
    return Value::String(std::move(out));
  });
  InstallNative(string_constructor, "fromCodePoint", [](NativeCall& call) {
    // Unlike fromCharCode this is not the inverse of an index: a code point
    // above U+FFFF is more than one unit however a string is stored, so it is
    // encoded rather than truncated.
    std::string out;
    if (call.arguments.size() > kMaxAllocationLength) {
      return call.Throw("RangeError", "string is too long");
    }
    for (const Value& argument : call.arguments) {
      const double value = ToNumber(argument);
      if (!std::isfinite(value) || value < 0.0 || value > 0x10FFFF ||
          value != std::trunc(value)) {
        return call.Throw("RangeError", "invalid code point");
      }
      util::AppendUtf8(out, static_cast<std::uint32_t>(value));
    }
    return Value::String(std::move(out));
  });
  InstallNative(string_constructor, "raw", [](NativeCall& call) {
    // The tag that undoes escape processing: it reads `.raw` off the strings
    // object a tagged template hands over, and joins the substitutions between
    // its entries.
    const Value strings = Argument(call.arguments, 0);
    if (!strings.IsObject()) {
      return call.Throw("TypeError", "String.raw needs a template strings object");
    }
    const Value raw = call.interpreter.GetPropertyValue(strings, "raw");
    if (!raw.IsObject()) {
      return Value::String(std::string());
    }
    std::string out;
    const std::size_t count = raw.object->ElementCount();
    for (std::size_t i = 0; i < count; ++i) {
      std::string chunk;
      const Result converted = call.interpreter.ToStringOf(raw.object->GetElement(i), chunk);
      if (converted.IsAbrupt()) {
        return call.ThrowValue(converted.value);
      }
      out += chunk;
      if (i + 1 < count && i + 1 < call.arguments.size()) {
        std::string substitution;
        const Result made =
            call.interpreter.ToStringOf(call.arguments[i + 1], substitution);
        if (made.IsAbrupt()) {
          return call.ThrowValue(made.value);
        }
        out += substitution;
      }
    }
    return Value::String(std::move(out));
  });
}

}  // namespace microbrowser::js
