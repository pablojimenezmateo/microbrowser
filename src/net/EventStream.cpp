#include "net/EventStream.h"

#include <algorithm>

#include "util/PerformanceCounters.h"
#include "util/Parse.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// A line, and how many bytes it took. The three terminators are all legal and a stream
// may mix them: `\r\n`, `\n`, `\r`. A parser that only knew one would treat a whole
// stream as a single unterminated line.
bool NextLine(std::string_view input, std::size_t at, std::string_view& line,
              std::size_t& next) {
  if (at >= input.size()) {
    return false;
  }
  const std::size_t cr = input.find('\r', at);
  const std::size_t lf = input.find('\n', at);
  if (cr == std::string_view::npos && lf == std::string_view::npos) {
    return false;  // no terminator yet: an incomplete line, not an empty one
  }
  if (cr != std::string_view::npos && (lf == std::string_view::npos || cr < lf)) {
    line = input.substr(at, cr - at);
    // `\r\n` is one terminator. Splitting it would produce a spurious blank line, which
    // in this format means "dispatch" -- so it would fire an event per line.
    next = (lf == cr + 1) ? cr + 2 : cr + 1;
    return true;
  }
  line = input.substr(at, lf - at);
  next = lf + 1;
  return true;
}

}  // namespace

EventStreamResult ParseEventStream(std::string_view input, std::size_t max_event) {
  EventStreamResult result;
  std::string type;
  std::string data;
  bool have_data = false;
  std::optional<std::string> id;
  bool overflowed = false;

  std::size_t at = 0;
  std::string_view line;
  std::size_t next = 0;
  while (NextLine(input, at, line, next)) {
    at = next;
    if (line.empty()) {
      // Dispatch. An event with no `data` field fires nothing -- that is what makes a
      // `:keep-alive` comment followed by a blank line cost nothing, and it is the
      // difference between a keep-alive and a message.
      if (have_data && !overflowed) {
        result.events.push_back(ServerSentEvent{type, data, id});
        AddPerformanceCounter(PerfCounterId::EventStreamEvents);
      }
      if (overflowed) {
        AddPerformanceCounter(PerfCounterId::EventStreamOversizeDrops);
      }
      type.clear();
      data.clear();
      have_data = false;
      overflowed = false;
      result.consumed = at;
      continue;
    }
    if (line.front() == ':') {
      continue;  // a comment, which is what a keep-alive is
    }
    const std::size_t colon = line.find(':');
    // A line with no colon is a field whose value is empty, not an error. `data` alone
    // is a legal event with an empty payload.
    const std::string_view field = colon == std::string_view::npos ? line : line.substr(0, colon);
    std::string_view value =
        colon == std::string_view::npos ? std::string_view() : line.substr(colon + 1);
    // Exactly one leading space, and only one: `data:  x` is " x", and stripping both
    // corrupts every payload a server indents.
    if (!value.empty() && value.front() == ' ') {
      value.remove_prefix(1);
    }

    if (field == "data") {
      if (have_data) {
        // Multiple `data:` lines are one payload joined by newlines. This is why an event
        // is not a line.
        data.push_back('\n');
      }
      if (data.size() + value.size() > max_event) {
        overflowed = true;
      } else {
        data.append(value);
      }
      have_data = true;
    } else if (field == "event") {
      type = std::string(value);
    } else if (field == "id") {
      // A NUL in an id is the one case the specification says to ignore the field
      // entirely rather than sanitise it, because the id goes back out in a header.
      if (value.find('\0') == std::string_view::npos) {
        id = std::string(value);
      }
    } else if (field == "retry") {
      if (const std::optional<int> parsed = util::ParseInt(value); parsed.has_value() &&
                                                                  *parsed >= 0) {
        result.retry_ms = static_cast<std::uint32_t>(*parsed);
      }
    }
    // Any other field is ignored, which is what the specification says and what lets a
    // server add one without breaking every client.
  }
  return result;
}

}  // namespace microbrowser::net
