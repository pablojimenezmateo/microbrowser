#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"

// `Date`.
//
// Its own translation unit, and it earns one: forty-five methods, a parser, a
// formatter, and the only arithmetic in the engine that is not about numbers.
//
// **The calendar is computed here rather than asked of the platform.** `std::tm`
// and `mktime` are limited to what `time_t` holds, use a global timezone
// setting, and disagree between platforms about what happens outside their
// range -- and a page can name the year 275760. The civil-from-days algorithm
// below is exact for every day the language allows, which is +/-100,000,000
// days from the epoch. The platform is asked exactly one question, and only
// about instants it can represent: what is the local offset from UTC.
//
// **The clock is a fingerprinting surface.** `Date.now()` at millisecond
// resolution is what every timing probe is built on. Millisecond resolution is
// what the spec requires and what is given; nothing finer is available from
// here, and `guidelines/privacy.md` is why.

namespace microbrowser::js {

namespace {

// The range the language allows: 8.64e15 milliseconds either side of the
// epoch. Anything outside is not a clamp but a NaN date, which is a value a
// page can hold and print.
constexpr double kMaxTime = 8.64e15;
constexpr double kMsPerSecond = 1000.0;
constexpr double kMsPerMinute = 60000.0;
constexpr double kMsPerHour = 3600000.0;
constexpr double kMsPerDay = 86400000.0;

// A calendar date, broken out. Fields rather than std::tm because two of them
// are wider than an int would make comfortable and because `std::tm`'s
// year-1900 and zero-based-month conventions are a source of off-by-ones that
// this file has no reason to inherit.
struct Civil {
  double year = 1970;
  int month = 0;  // 0-11, as the language counts them
  int day = 1;    // 1-31
  int weekday = 4;  // 0 = Sunday; 1970-01-01 was a Thursday
  int hour = 0;
  int minute = 0;
  int second = 0;
  int millisecond = 0;
};

// Floor division and modulo, which are what date arithmetic needs and what C++
// does not give: `-1 / 2` is 0 and `-1 % 2` is -1, and both are the wrong
// answer for an instant before the epoch.
double FloorDiv(double a, double b) { return std::floor(a / b); }
double FloorMod(double a, double b) { return a - b * std::floor(a / b); }

// Howard Hinnant's civil-from-days, which is exact over the whole range and
// short enough to check by eye. `days` is days since 1970-01-01.
void CivilFromDays(double days, double& year, int& month, int& day) {
  const double shifted = days + 719468;
  const double era = FloorDiv(shifted, 146097);
  const double day_of_era = shifted - era * 146097;  // [0, 146096]
  const double year_of_era = std::floor(
      (day_of_era - FloorDiv(day_of_era, 1460) + FloorDiv(day_of_era, 36524) -
       FloorDiv(day_of_era, 146096)) /
      365);  // [0, 399]
  const double y = year_of_era + era * 400;
  const double day_of_year =
      day_of_era - (365 * year_of_era + FloorDiv(year_of_era, 4) - FloorDiv(year_of_era, 100));
  const double mp = std::floor((5 * day_of_year + 2) / 153);  // [0, 11], March-based
  day = static_cast<int>(day_of_year - std::floor((153 * mp + 2) / 5) + 1);
  const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);  // [1, 12]
  month = m - 1;
  year = m <= 2 ? y + 1 : y;
}

// The inverse, and the reason the two are written as a pair: `setMonth` runs
// one and then the other, and a rounding difference between them would move
// the day.
double DaysFromCivil(double year, int month, int day) {
  // A month outside 0-11 rolls the year, which is what `new Date(2020, 12, 1)`
  // meaning January 2021 depends on.
  year += FloorDiv(month, 12);
  const int m = static_cast<int>(FloorMod(month, 12)) + 1;  // [1, 12]
  const double y = m <= 2 ? year - 1 : year;
  const double era = FloorDiv(y, 400);
  const double year_of_era = y - era * 400;
  const double day_of_year =
      std::floor((153 * (m > 2 ? m - 3 : m + 9) + 2) / 5) + day - 1;
  const double day_of_era =
      year_of_era * 365 + FloorDiv(year_of_era, 4) - FloorDiv(year_of_era, 100) + day_of_year;
  return era * 146097 + day_of_era - 719468;
}

Civil Break(double time) {
  Civil out;
  const double days = FloorDiv(time, kMsPerDay);
  CivilFromDays(days, out.year, out.month, out.day);
  // 1970-01-01 was a Thursday, hence the 4. FloorMod rather than `%` so that a
  // date before the epoch does not come back negative.
  out.weekday = static_cast<int>(FloorMod(days + 4, 7));
  const double in_day = FloorMod(time, kMsPerDay);
  out.hour = static_cast<int>(FloorDiv(in_day, kMsPerHour));
  out.minute = static_cast<int>(FloorMod(FloorDiv(in_day, kMsPerMinute), 60));
  out.second = static_cast<int>(FloorMod(FloorDiv(in_day, kMsPerSecond), 60));
  out.millisecond = static_cast<int>(FloorMod(in_day, kMsPerSecond));
  return out;
}

double Join(const Civil& parts) {
  return DaysFromCivil(parts.year, parts.month, parts.day) * kMsPerDay +
         parts.hour * kMsPerHour + parts.minute * kMsPerMinute +
         parts.second * kMsPerSecond + parts.millisecond;
}

// Anything outside the representable range, or not a number at all, is an
// invalid date -- which is a value rather than an error, and prints as such.
double Clip(double time) {
  if (!std::isfinite(time) || std::fabs(time) > kMaxTime) {
    return std::nan("");
  }
  return std::trunc(time) + 0.0;  // +0.0 so that -0 does not survive
}

// The local timezone's offset from UTC at `time`, in milliseconds.
//
// The one question the platform is asked, and it is asked about an instant
// rather than about a rule: the offset changes twice a year in most of the
// world, so a fixed offset read once would be an hour wrong for half the year.
// Outside what `time_t` can hold there is no answer to be had, and the offset
// at the nearest representable instant is used instead -- which is right
// unless a page is asking about daylight saving in the year 300,000.
double LocalOffset(double time) {
  if (std::isnan(time)) {
    return 0.0;
  }
  const double seconds = std::floor(time / kMsPerSecond);
  // Clamped so the conversion below is defined. The two bounds are chosen to
  // sit well inside a 64-bit time_t and well outside any date a page means.
  const double clamped = std::min(std::max(seconds, -67768036191676800.0 / 1000.0),
                                  67768036191676800.0 / 1000.0);
  const auto stamp = static_cast<std::time_t>(clamped);
  std::tm local{};
  std::tm utc{};
#if defined(_WIN32)
  localtime_s(&local, &stamp);
  gmtime_s(&utc, &stamp);
#else
  localtime_r(&stamp, &local);
  gmtime_r(&stamp, &utc);
#endif
  // The difference between the two breakdowns of the same instant, computed
  // through the calendar above rather than through mktime -- which would
  // re-apply the timezone this is trying to measure.
  const double local_ms =
      DaysFromCivil(local.tm_year + 1900.0, local.tm_mon, local.tm_mday) * kMsPerDay +
      local.tm_hour * kMsPerHour + local.tm_min * kMsPerMinute + local.tm_sec * kMsPerSecond;
  const double utc_ms =
      DaysFromCivil(utc.tm_year + 1900.0, utc.tm_mon, utc.tm_mday) * kMsPerDay +
      utc.tm_hour * kMsPerHour + utc.tm_min * kMsPerMinute + utc.tm_sec * kMsPerSecond;
  return local_ms - utc_ms;
}

// The instant a Date object holds, or NaN for anything that is not one.
double TimeOf(const Value& value) {
  if (!value.IsObject()) {
    return std::nan("");
  }
  const Value* stored = value.object->GetOwn("#time");
  return stored == nullptr ? std::nan("") : ToNumber(*stored);
}

std::string Pad(double value, int width) {
  char buffer[32];
  const bool negative = value < 0;
  std::snprintf(buffer, sizeof(buffer), "%0*.0f", width, std::fabs(value));
  return negative ? "-" + std::string(buffer) : std::string(buffer);
}

const char* kWeekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// --- Parsing ----------------------------------------------------------------
//
// Two formats, and no more. The ISO 8601 subset the spec defines, which is what
// every API and every serialized date uses; and the "Thu Jan 01 1970" form the
// spec's own `toString` produces, so that a round trip works. Anything else is
// NaN -- a wrong date is worse than an admitted failure, and the alternative is
// the format zoo every other engine has accumulated and cannot now drop.

bool ReadDigits(std::string_view text, std::size_t& at, int count, double& out) {
  if (at + static_cast<std::size_t>(count) > text.size()) {
    return false;
  }
  double value = 0;
  for (int i = 0; i < count; ++i) {
    const char c = text[at + static_cast<std::size_t>(i)];
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  at += static_cast<std::size_t>(count);
  out = value;
  return true;
}

double ParseIso(std::string_view text) {
  std::size_t at = 0;
  Civil parts;
  double sign = 1.0;
  int year_digits = 4;
  if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
    // The extended year form, which is six digits and signed.
    sign = text[at] == '-' ? -1.0 : 1.0;
    ++at;
    year_digits = 6;
  }
  if (!ReadDigits(text, at, year_digits, parts.year)) {
    return std::nan("");
  }
  parts.year *= sign;
  // A date-only form is UTC; a date-time form without a zone is *local*. That
  // asymmetry is the spec's and pages depend on both halves of it.
  bool date_only = true;
  if (at < text.size() && text[at] == '-') {
    ++at;
    double month = 0;
    if (!ReadDigits(text, at, 2, month)) {
      return std::nan("");
    }
    parts.month = static_cast<int>(month) - 1;
    if (at < text.size() && text[at] == '-') {
      ++at;
      double day = 0;
      if (!ReadDigits(text, at, 2, day)) {
        return std::nan("");
      }
      parts.day = static_cast<int>(day);
    }
  }
  bool has_zone = false;
  double offset = 0;
  if (at < text.size() && (text[at] == 'T' || text[at] == ' ')) {
    date_only = false;
    ++at;
    double hour = 0;
    double minute = 0;
    if (!ReadDigits(text, at, 2, hour) || at >= text.size() || text[at] != ':') {
      return std::nan("");
    }
    ++at;
    if (!ReadDigits(text, at, 2, minute)) {
      return std::nan("");
    }
    parts.hour = static_cast<int>(hour);
    parts.minute = static_cast<int>(minute);
    if (at < text.size() && text[at] == ':') {
      ++at;
      double second = 0;
      if (!ReadDigits(text, at, 2, second)) {
        return std::nan("");
      }
      parts.second = static_cast<int>(second);
      if (at < text.size() && text[at] == '.') {
        ++at;
        // Any number of fractional digits; the first three are milliseconds
        // and the rest are truncated, which is what every other engine does.
        double scale = 100;
        double fraction = 0;
        std::size_t digits = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
          if (digits < 3) {
            fraction += (text[at] - '0') * scale;
            scale /= 10;
          }
          ++digits;
          ++at;
        }
        if (digits == 0) {
          return std::nan("");
        }
        parts.millisecond = static_cast<int>(fraction);
      }
    }
    if (at < text.size() && (text[at] == 'Z' || text[at] == 'z')) {
      has_zone = true;
      ++at;
    } else if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
      const double zone_sign = text[at] == '-' ? -1.0 : 1.0;
      ++at;
      double hours = 0;
      double minutes = 0;
      if (!ReadDigits(text, at, 2, hours)) {
        return std::nan("");
      }
      if (at < text.size() && text[at] == ':') {
        ++at;
      }
      if (!ReadDigits(text, at, 2, minutes)) {
        return std::nan("");
      }
      offset = zone_sign * (hours * kMsPerHour + minutes * kMsPerMinute);
      has_zone = true;
    }
  }
  if (at != text.size()) {
    return std::nan("");
  }
  const double utc = Join(parts) - offset;
  if (has_zone || date_only) {
    return Clip(utc);
  }
  // No zone on a date-time: the fields are local. The offset has to be the one
  // in effect at that instant, and the instant is what is being computed --
  // so it is estimated from the naive value and applied once, which is exact
  // except in the hour a transition moves.
  return Clip(utc - LocalOffset(utc));
}

// The `Thu Jan 01 1970 00:00:00 GMT+0000` form, which is what `toString`
// produces. Parsed so that `new Date(String(d))` round-trips.
double ParseTextual(std::string_view text) {
  std::size_t at = 0;
  const auto skip_spaces = [&] {
    while (at < text.size() && text[at] == ' ') {
      ++at;
    }
  };
  const auto word = [&](std::string_view& out) {
    const std::size_t start = at;
    while (at < text.size() && text[at] != ' ') {
      ++at;
    }
    out = text.substr(start, at - start);
    return at != start;
  };

  std::string_view token;
  skip_spaces();
  if (!word(token)) {
    return std::nan("");
  }
  // An optional weekday, which carries no information and is skipped.
  for (const char* name : kWeekdays) {
    if (token == name) {
      skip_spaces();
      if (!word(token)) {
        return std::nan("");
      }
      break;
    }
  }
  Civil parts;
  int month = -1;
  for (int i = 0; i < 12; ++i) {
    if (token == kMonths[i]) {
      month = i;
    }
  }
  if (month < 0) {
    return std::nan("");
  }
  parts.month = month;
  skip_spaces();
  std::size_t scan = at;
  double day = 0;
  if (!ReadDigits(text, scan, 2, day)) {
    return std::nan("");
  }
  at = scan;
  parts.day = static_cast<int>(day);
  skip_spaces();
  scan = at;
  if (!ReadDigits(text, scan, 4, parts.year)) {
    return std::nan("");
  }
  at = scan;
  double offset = 0;
  bool has_zone = false;
  skip_spaces();
  if (at < text.size()) {
    scan = at;
    double hour = 0;
    double minute = 0;
    double second = 0;
    if (ReadDigits(text, scan, 2, hour) && scan < text.size() && text[scan] == ':') {
      ++scan;
      if (!ReadDigits(text, scan, 2, minute)) {
        return std::nan("");
      }
      if (scan < text.size() && text[scan] == ':') {
        ++scan;
        if (!ReadDigits(text, scan, 2, second)) {
          return std::nan("");
        }
      }
      parts.hour = static_cast<int>(hour);
      parts.minute = static_cast<int>(minute);
      parts.second = static_cast<int>(second);
      at = scan;
    }
    skip_spaces();
    if (text.substr(at, 3) == "GMT" || text.substr(at, 3) == "UTC") {
      at += 3;
      has_zone = true;
      if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
        const double zone_sign = text[at] == '-' ? -1.0 : 1.0;
        ++at;
        double hours = 0;
        double minutes = 0;
        if (!ReadDigits(text, at, 2, hours) || !ReadDigits(text, at, 2, minutes)) {
          return std::nan("");
        }
        offset = zone_sign * (hours * kMsPerHour + minutes * kMsPerMinute);
      }
    }
  }
  // Trailing text -- a timezone name in parentheses is what `toString` adds --
  // is ignored rather than refused: it carries nothing the offset did not.
  const double utc = Join(parts) - offset;
  return Clip(has_zone ? utc : utc - LocalOffset(utc));
}

double ParseDate(std::string_view text) {
  while (!text.empty() && text.front() == ' ') {
    text.remove_prefix(1);
  }
  while (!text.empty() && text.back() == ' ') {
    text.remove_suffix(1);
  }
  const double iso = ParseIso(text);
  return std::isnan(iso) ? ParseTextual(text) : iso;
}

}  // namespace

void Interpreter::InstallDate() {
  Object* date_prototype = NewObject();
  Object* date = NewNative("Date", [](NativeCall& call) {
    Object* instance = call.interpreter.GetHeap().AllocateObject(Object::Kind::Plain);
    if (instance == nullptr) {
      return call.Throw("RangeError", "out of memory");
    }
    const Value* prototype = call.callee == nullptr ? nullptr : call.callee->GetOwn("prototype");
    if (prototype != nullptr && prototype->IsObject()) {
      instance->SetPrototype(prototype->object);
    }
    double milliseconds = 0.0;
    if (call.arguments.empty()) {
      milliseconds = call.interpreter.NowMilliseconds();
    } else if (call.arguments.size() == 1) {
      // One argument is either an instant or a string -- and which it is
      // depends on what it converts to, not on its type, because a Date passed
      // here has to copy rather than be re-parsed from its own printed form.
      Value primitive;
      const Result converted = call.interpreter.ToPrimitive(
          call.arguments[0], Interpreter::Hint::Default, primitive);
      if (converted.IsAbrupt()) {
        return call.ThrowValue(converted.value);
      }
      milliseconds = primitive.IsString() ? ParseDate(primitive.AsString())
                                          : Clip(ToNumber(primitive));
    } else {
      // Fields, in local time. A two-digit year means 19xx, which is a rule
      // from 1995 that the web still depends on.
      Civil parts;
      parts.year = ToNumber(call.arguments[0]);
      if (parts.year >= 0 && parts.year <= 99 && parts.year == std::trunc(parts.year)) {
        parts.year += 1900;
      }
      const auto field = [&call](std::size_t index, double fallback) {
        return index < call.arguments.size() ? ToNumber(call.arguments[index]) : fallback;
      };
      const double month = field(1, 0);
      const double day = field(2, 1);
      const double hour = field(3, 0);
      const double minute = field(4, 0);
      const double second = field(5, 0);
      const double ms = field(6, 0);
      if (!std::isfinite(parts.year) || !std::isfinite(month) || !std::isfinite(day) ||
          !std::isfinite(hour) || !std::isfinite(minute) || !std::isfinite(second) ||
          !std::isfinite(ms)) {
        milliseconds = std::nan("");
      } else {
        // Out-of-range fields roll rather than fail: `new Date(2020, 0, 32)` is
        // the first of February, and pages add days that way on purpose.
        const double naive = DaysFromCivil(parts.year, static_cast<int>(std::trunc(month)),
                                           static_cast<int>(std::trunc(day))) *
                                 kMsPerDay +
                             std::trunc(hour) * kMsPerHour + std::trunc(minute) * kMsPerMinute +
                             std::trunc(second) * kMsPerSecond + std::trunc(ms);
        milliseconds = Clip(naive - LocalOffset(naive));
      }
    }
    instance->Set("#time", Value::Number(milliseconds));
    return Value::Obj(instance);
  });
  if (date == nullptr || date_prototype == nullptr) {
    return;
  }
  date_prototype->SetPrototype(well_known_.object_prototype);
  date->Set("prototype", Value::Obj(date_prototype));
  date_prototype->Set("constructor", Value::Obj(date));
  global_scope_->Declare("Date", Value::Obj(date), false);

  InstallNative(date, "now",
                [](NativeCall& call) { return Value::Number(call.interpreter.NowMilliseconds()); });
  InstallNative(date, "parse", [](NativeCall& call) {
    std::string text;
    const Result converted = call.interpreter.ToStringOf(Argument(call.arguments, 0), text);
    if (converted.IsAbrupt()) {
      return call.ThrowValue(converted.value);
    }
    return Value::Number(ParseDate(text));
  });
  InstallNative(date, "UTC", [](NativeCall& call) {
    // The same fields the constructor takes, read as UTC rather than local.
    Civil parts;
    parts.year = ToNumber(Argument(call.arguments, 0));
    if (parts.year >= 0 && parts.year <= 99 && parts.year == std::trunc(parts.year)) {
      parts.year += 1900;
    }
    const auto field = [&call](std::size_t index, double fallback) {
      return index < call.arguments.size() ? ToNumber(call.arguments[index]) : fallback;
    };
    const double month = field(1, 0);
    const double day = field(2, 1);
    const double hour = field(3, 0);
    const double minute = field(4, 0);
    const double second = field(5, 0);
    const double ms = field(6, 0);
    if (!std::isfinite(parts.year) || !std::isfinite(month) || !std::isfinite(day) ||
        !std::isfinite(hour) || !std::isfinite(minute) || !std::isfinite(second) ||
        !std::isfinite(ms)) {
      return Value::Number(std::nan(""));
    }
    return Value::Number(Clip(DaysFromCivil(parts.year, static_cast<int>(std::trunc(month)),
                                            static_cast<int>(std::trunc(day))) *
                                  kMsPerDay +
                              std::trunc(hour) * kMsPerHour + std::trunc(minute) * kMsPerMinute +
                              std::trunc(second) * kMsPerSecond + std::trunc(ms)));
  });

  // --- The getters ----------------------------------------------------------
  //
  // Two of each: one reading local time and one reading UTC. Written once and
  // installed twice, with the offset being the whole difference between them --
  // which is what keeps `getHours` and `getUTCHours` from drifting apart.
  const auto reader = [this, date_prototype](const char* local_name, const char* utc_name,
                                             int (*pick)(const Civil&)) {
    for (const bool utc : {false, true}) {
      InstallNative(date_prototype, utc ? utc_name : local_name,
                    [pick, utc](NativeCall& call) {
                      const double time = TimeOf(call.self);
                      if (std::isnan(time)) {
                        return Value::Number(time);
                      }
                      const Civil parts = Break(utc ? time : time + LocalOffset(time));
                      return Value::Number(pick(parts));
                    });
    }
  };
  reader("getMonth", "getUTCMonth", [](const Civil& c) { return c.month; });
  reader("getDate", "getUTCDate", [](const Civil& c) { return c.day; });
  reader("getDay", "getUTCDay", [](const Civil& c) { return c.weekday; });
  reader("getHours", "getUTCHours", [](const Civil& c) { return c.hour; });
  reader("getMinutes", "getUTCMinutes", [](const Civil& c) { return c.minute; });
  reader("getSeconds", "getUTCSeconds", [](const Civil& c) { return c.second; });
  reader("getMilliseconds", "getUTCMilliseconds",
         [](const Civil& c) { return c.millisecond; });
  // The year is a double rather than an int -- the range runs to 275760 -- so
  // it does not fit the helper above.
  for (const bool utc : {false, true}) {
    InstallNative(date_prototype, utc ? "getUTCFullYear" : "getFullYear",
                  [utc](NativeCall& call) {
                    const double time = TimeOf(call.self);
                    if (std::isnan(time)) {
                      return Value::Number(time);
                    }
                    return Value::Number(Break(utc ? time : time + LocalOffset(time)).year);
                  });
  }
  // Legacy, and still called: the year minus 1900, which is why the year 2000
  // was written "100" in a great deal of code.
  InstallNative(date_prototype, "getYear", [](NativeCall& call) {
    const double time = TimeOf(call.self);
    if (std::isnan(time)) {
      return Value::Number(time);
    }
    return Value::Number(Break(time + LocalOffset(time)).year - 1900);
  });

  const auto instant = [](NativeCall& call) { return Value::Number(TimeOf(call.self)); };
  InstallNative(date_prototype, "getTime", instant);
  InstallNative(date_prototype, "valueOf", instant);
  InstallNative(date_prototype, "getTimezoneOffset", [](NativeCall& call) {
    const double time = TimeOf(call.self);
    if (std::isnan(time)) {
      return Value::Number(time);
    }
    // Minutes, and with the sign inverted from what the name suggests: a zone
    // *ahead* of UTC reports a negative offset. The spec's convention, and
    // every page that formats a date depends on it.
    return Value::Number(-LocalOffset(time) / kMsPerMinute);
  });

  // --- The setters ----------------------------------------------------------
  //
  // Each writes a run of fields starting at one position, which is what makes
  // `setHours(h, m, s, ms)` one function rather than four. Field indices:
  // 0 year, 1 month, 2 day, 3 hour, 4 minute, 5 second, 6 millisecond.
  const auto writer = [this, date_prototype](const char* local_name, const char* utc_name,
                                             int first, int count) {
    for (const bool utc : {false, true}) {
      InstallNative(
          date_prototype, utc ? utc_name : local_name,
          [first, count, utc](NativeCall& call) {
            if (!call.self.IsObject()) {
              return Value::Number(std::nan(""));
            }
            const double time = TimeOf(call.self);
            // Setting a field of an invalid date leaves it invalid -- except
            // setFullYear, which the spec lets start from the epoch so that a
            // date can be rebuilt from nothing.
            if (std::isnan(time) && first != 0) {
              call.self.object->Set("#time", Value::Number(std::nan("")));
              return Value::Number(std::nan(""));
            }
            const double base = std::isnan(time) ? 0.0 : time;
            const double offset = utc ? 0.0 : LocalOffset(base);
            Civil parts = Break(base + offset);
            double fields[7] = {parts.year,
                                static_cast<double>(parts.month),
                                static_cast<double>(parts.day),
                                static_cast<double>(parts.hour),
                                static_cast<double>(parts.minute),
                                static_cast<double>(parts.second),
                                static_cast<double>(parts.millisecond)};
            bool invalid = false;
            for (int i = 0; i < count && static_cast<std::size_t>(i) < call.arguments.size();
                 ++i) {
              const double value = ToNumber(call.arguments[static_cast<std::size_t>(i)]);
              if (!std::isfinite(value)) {
                invalid = true;
                break;
              }
              fields[first + i] = std::trunc(value);
            }
            double updated = std::nan("");
            if (!invalid) {
              const double naive =
                  DaysFromCivil(fields[0], static_cast<int>(fields[1]),
                                static_cast<int>(fields[2])) *
                      kMsPerDay +
                  fields[3] * kMsPerHour + fields[4] * kMsPerMinute +
                  fields[5] * kMsPerSecond + fields[6];
              // The offset is re-read at the new instant, not reused from the
              // old one: setting a date across a daylight-saving boundary has
              // to land on the wall-clock time that was asked for.
              updated = Clip(utc ? naive : naive - LocalOffset(naive - offset));
            }
            call.self.object->Set("#time", Value::Number(updated));
            return Value::Number(updated);
          });
    }
  };
  writer("setFullYear", "setUTCFullYear", 0, 3);
  writer("setMonth", "setUTCMonth", 1, 2);
  writer("setDate", "setUTCDate", 2, 1);
  writer("setHours", "setUTCHours", 3, 4);
  writer("setMinutes", "setUTCMinutes", 4, 3);
  writer("setSeconds", "setUTCSeconds", 5, 2);
  writer("setMilliseconds", "setUTCMilliseconds", 6, 1);
  InstallNative(date_prototype, "setTime", [](NativeCall& call) {
    const double time = Clip(ToNumber(Argument(call.arguments, 0)));
    if (call.self.IsObject()) {
      call.self.object->Set("#time", Value::Number(time));
    }
    return Value::Number(time);
  });

  // --- Printing -------------------------------------------------------------

  InstallNative(date_prototype, "toISOString", [](NativeCall& call) {
    const double time = TimeOf(call.self);
    if (std::isnan(time)) {
      return call.Throw("RangeError", "an invalid date has no ISO form");
    }
    const Civil parts = Break(time);
    // A year outside four digits takes the extended form, which is signed and
    // six digits -- and is what keeps the output parseable by Date.parse.
    const std::string year = parts.year >= 0 && parts.year <= 9999
                                 ? Pad(parts.year, 4)
                                 : (parts.year > 0 ? "+" : "-") + Pad(std::fabs(parts.year), 6);
    return Value::String(year + "-" + Pad(parts.month + 1, 2) + "-" + Pad(parts.day, 2) + "T" +
                         Pad(parts.hour, 2) + ":" + Pad(parts.minute, 2) + ":" +
                         Pad(parts.second, 2) + "." + Pad(parts.millisecond, 3) + "Z");
  });
  InstallNative(date_prototype, "toJSON", [](NativeCall& call) {
    // Null for an invalid date rather than a throw, which is what makes
    // `JSON.stringify({d: new Date(NaN)})` produce a document at all.
    if (std::isnan(TimeOf(call.self))) {
      return Value::Null();
    }
    const Value method = call.interpreter.GetPropertyValue(call.self, "toISOString");
    const Result text = call.interpreter.CallFunction(method, call.self, {});
    return text.IsAbrupt() ? call.ThrowValue(text.value) : text.value;
  });

  // The three halves of `toString`, so that `toDateString` and `toTimeString`
  // are the same text this produced rather than a second formatting of it.
  const auto date_part = [](const Civil& parts) {
    return std::string(kWeekdays[parts.weekday]) + " " + kMonths[parts.month] + " " +
           Pad(parts.day, 2) + " " +
           (parts.year >= 0 ? Pad(parts.year, 4) : "-" + Pad(std::fabs(parts.year), 6));
  };
  const auto time_part = [](const Civil& parts, double offset) {
    const double minutes = offset / kMsPerMinute;
    const std::string sign = minutes < 0 ? "-" : "+";
    return Pad(parts.hour, 2) + ":" + Pad(parts.minute, 2) + ":" + Pad(parts.second, 2) +
           " GMT" + sign + Pad(std::floor(std::fabs(minutes) / 60), 2) +
           Pad(std::fmod(std::fabs(minutes), 60), 2);
  };
  const auto printer = [this, date_prototype, date_part, time_part](const char* name,
                                                                    int which) {
    InstallNative(date_prototype, name, [date_part, time_part, which](NativeCall& call) {
      const double time = TimeOf(call.self);
      if (std::isnan(time)) {
        return Value::String(std::string("Invalid Date"));
      }
      const double offset = LocalOffset(time);
      const Civil parts = Break(time + offset);
      if (which == 0) {
        return Value::String(date_part(parts) + " " + time_part(parts, offset));
      }
      return Value::String(which == 1 ? date_part(parts) : time_part(parts, offset));
    });
  };
  printer("toString", 0);
  printer("toDateString", 1);
  printer("toTimeString", 2);
  // No locale data, so the locale spellings are the plain ones. Standing in a
  // fabricated format would be a lie a page cannot detect, and the plain form
  // at least says what the date is.
  printer("toLocaleString", 0);
  printer("toLocaleDateString", 1);
  printer("toLocaleTimeString", 2);
  InstallNative(date_prototype, "toUTCString", [](NativeCall& call) {
    const double time = TimeOf(call.self);
    if (std::isnan(time)) {
      return Value::String(std::string("Invalid Date"));
    }
    const Civil parts = Break(time);
    // The HTTP-date form, which is what a page building a header wants.
    return Value::String(std::string(kWeekdays[parts.weekday]) + ", " + Pad(parts.day, 2) + " " +
                         kMonths[parts.month] + " " + Pad(parts.year, 4) + " " +
                         Pad(parts.hour, 2) + ":" + Pad(parts.minute, 2) + ":" +
                         Pad(parts.second, 2) + " GMT");
  });
  InstallNative(date_prototype, "toGMTString", [](NativeCall& call) {
    const Value method = call.interpreter.GetPropertyValue(call.self, "toUTCString");
    const Result text = call.interpreter.CallFunction(method, call.self, {});
    return text.IsAbrupt() ? call.ThrowValue(text.value) : text.value;
  });

  // The one exotic conversion in the language: a Date prefers its *string*
  // under the default hint, which is why `date + 1` concatenates and
  // `date - 1` subtracts. Nothing else in the language does this, and it is
  // the reason Symbol.toPrimitive exists at all.
  if (well_known_.symbol_to_primitive != nullptr) {
    Object* to_primitive = NewNative("[Symbol.toPrimitive]", [](NativeCall& call) {
      const std::string hint = ToString(Argument(call.arguments, 0));
      const char* method = hint == "number" ? "valueOf" : "toString";
      const Value function = call.interpreter.GetPropertyValue(call.self, method);
      const Result out = call.interpreter.CallFunction(function, call.self, {});
      return out.IsAbrupt() ? call.ThrowValue(out.value) : out.value;
    });
    if (to_primitive != nullptr) {
      date_prototype->Set(PropertyKey::Symbol(well_known_.symbol_to_primitive),
                          Value::Obj(to_primitive));
    }
  }
}

}  // namespace microbrowser::js
