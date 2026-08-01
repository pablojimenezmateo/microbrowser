#include "util/PerformanceTrace.h"

#include "util/Env.h"

namespace microbrowser::util {

TraceChannel& PerformanceTrace::Channel() {
  // Function-local so the env read happens on first use rather than during
  // static initialization of an arbitrary translation unit.
  static TraceChannel channel("perf", "MICROBROWSER_PERF_TRACE", "MICROBROWSER_PERF_SUMMARY",
                              "MICROBROWSER_PERF_TRACE_MIN_MS");
  return channel;
}

PerformanceTrace::ScopeLabel::ScopeLabel(TraceChannel& channel, std::string_view base) {
  if (!channel.Enabled()) {
    return;
  }
  enabled_ = true;
  text_.assign(base);
}

PerformanceTrace::ScopeLabel& PerformanceTrace::ScopeLabel::Field(std::string_view key,
                                                                 std::string_view value) {
  if (!enabled_) {
    return *this;
  }
  text_ += open_ ? ',' : '(';
  open_ = true;
  text_.append(key);
  text_ += '=';
  text_.append(value);
  return *this;
}

PerformanceTrace::ScopeLabel& PerformanceTrace::ScopeLabel::Field(std::string_view key,
                                                                 long long value) {
  if (!enabled_) {
    return *this;
  }
  const std::string text = std::to_string(value);
  return Field(key, std::string_view(text));
}

PerformanceTrace::ScopeLabel& PerformanceTrace::ScopeLabel::FieldPath(
    std::string_view key,
    const std::filesystem::path& value) {
  if (!enabled_) {
    return *this;
  }
  if constexpr (std::is_same_v<std::filesystem::path::value_type, char>) {
    // POSIX: native() is the stored buffer, so this is a view, not a copy.
    return Field(key, std::string_view(value.native()));
  } else {
    const std::string text = value.string();
    return Field(key, std::string_view(text));
  }
}

std::string_view PerformanceTrace::ScopeLabel::View() {
  if (open_) {
    text_ += ')';
    open_ = false;
  }
  return text_;
}

bool PerformanceTrace::FlagEnabled(const char* env_name) { return EnvFlagEnabled(env_name); }

}  // namespace microbrowser::util
