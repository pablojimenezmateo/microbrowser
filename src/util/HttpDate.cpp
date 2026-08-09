#include "util/HttpDate.h"

#include <cctype>
#include <string>

#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microbrowser::util {

namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t'; }

void SkipSpaces(std::string_view& text) {
  while (!text.empty() && IsSpace(text.front())) {
    text.remove_prefix(1);
  }
}

std::optional<std::string_view> TakeToken(std::string_view& text) {
  SkipSpaces(text);
  if (text.empty()) {
    return std::nullopt;
  }
  std::size_t end = 0;
  while (end < text.size() && !IsSpace(text[end]) && text[end] != ',') {
    ++end;
  }
  if (end == 0) {
    return std::nullopt;
  }
  const std::string_view token = text.substr(0, end);
  text.remove_prefix(end);
  return token;
}

std::optional<int> MonthIndex(std::string_view name) {
  static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  for (int i = 0; i < 12; ++i) {
    if (EqualsAsciiCaseInsensitive(name, kMonths[i])) {
      return i;
    }
  }
  return std::nullopt;
}

// Howard Hinnant's days-from-civil: exact, and the same algorithm `src/js`
// uses for Date, so a cookie Expires and a `Date` built from the same string
// agree on the instant.
std::int64_t DaysFromCivil(int year, int month, int day) {
  year += month / 12;
  month %= 12;
  if (month < 0) {
    month += 12;
    --year;
  }
  const int m = month + 1;  // [1, 12]
  const int y = m <= 2 ? year - 1 : year;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + day - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<std::int64_t>(era) * 146097 + doe - 719468;
}

std::optional<std::int64_t> ParseTimeOfDay(std::string_view token, int& hour, int& minute,
                                           int& second) {
  // HH:MM:SS
  if (token.size() != 8 || token[2] != ':' || token[5] != ':') {
    return std::nullopt;
  }
  const auto h = ParseInt64(token.substr(0, 2));
  const auto m = ParseInt64(token.substr(3, 2));
  const auto s = ParseInt64(token.substr(6, 2));
  if (!h.has_value() || !m.has_value() || !s.has_value()) {
    return std::nullopt;
  }
  if (*h < 0 || *h > 23 || *m < 0 || *m > 59 || *s < 0 || *s > 60) {
    return std::nullopt;
  }
  hour = static_cast<int>(*h);
  minute = static_cast<int>(*m);
  second = static_cast<int>(*s);
  return 0;
}

}  // namespace

std::optional<std::int64_t> ParseHttpDate(std::string_view text) {
  // Trim, then accept IMF-fix:
  //   day-name "," SP date1 SP time-of-day SP GMT
  //   date1 = day SP month SP year
  // e.g. Wed, 09 Nov 1994 08:49:37 GMT
  while (!text.empty() && IsSpace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsSpace(text.back())) {
    text.remove_suffix(1);
  }
  if (text.empty()) {
    return std::nullopt;
  }

  // Optional weekday + comma (required by IMF-fix; tolerate its absence).
  if (const auto comma = text.find(','); comma != std::string_view::npos) {
    text.remove_prefix(comma + 1);
  }

  const auto day_tok = TakeToken(text);
  const auto month_tok = TakeToken(text);
  const auto year_tok = TakeToken(text);
  const auto time_tok = TakeToken(text);
  const auto zone_tok = TakeToken(text);
  if (!day_tok || !month_tok || !year_tok || !time_tok || !zone_tok) {
    return std::nullopt;
  }
  SkipSpaces(text);
  if (!text.empty()) {
    return std::nullopt;
  }
  if (!EqualsAsciiCaseInsensitive(*zone_tok, "GMT")) {
    return std::nullopt;
  }

  const auto day = ParseInt64(*day_tok);
  const auto year = ParseInt64(*year_tok);
  const auto month = MonthIndex(*month_tok);
  if (!day || !year || !month || *day < 1 || *day > 31 || *year < 1601) {
    return std::nullopt;
  }
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!ParseTimeOfDay(*time_tok, hour, minute, second)) {
    return std::nullopt;
  }

  const std::int64_t days =
      DaysFromCivil(static_cast<int>(*year), *month, static_cast<int>(*day));
  return days * 86400 + static_cast<std::int64_t>(hour) * 3600 +
         static_cast<std::int64_t>(minute) * 60 + second;
}

}  // namespace microbrowser::util
