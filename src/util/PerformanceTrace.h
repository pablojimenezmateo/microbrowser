#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <type_traits>
#include <string_view>
#include <vector>

#include "util/TraceChannel.h"

namespace microbrowser::util {

// Runtime scope tracer for the live app.
//
// Two independent modes, both off by default:
//
//   MICROBROWSER_PERF_TRACE=1     stream one stderr line per scope, indented by
//                             nesting depth, filtered by
//                             MICROBROWSER_PERF_TRACE_MIN_MS.
//   MICROBROWSER_PERF_SUMMARY=1   accumulate per-label count/total/self/max and
//                             print one table ranked by self time at shutdown.
//
// Prefer the summary when hunting a hotspot: streaming does a write+flush inside
// the parent scope, so it distorts exactly the numbers you are reading.
class PerformanceTrace {
 public:
  static TraceChannel& Channel();

  static bool Enabled() { return Channel().Enabled(); }
  static bool SummaryEnabled() { return Channel().AggregateEnabled(); }
  // True only in streaming mode. Guard a bespoke stderr line with this rather
  // than Enabled(), or a summary-only run pays for output it never wanted.
  static bool StreamEnabled() { return Channel().StreamEnabled(); }
  static double MinimumDurationMs() { return Channel().MinimumDurationMs(); }

  // Generic env-flag reader for the adjacent opt-in trace flags
  // (MICROBROWSER_TRACE_REDRAW and friends).
  static bool FlagEnabled(const char* env_name);

  // Fold an already-measured duration into the ranked summary. See
  // TraceChannel::RecordSample.
  static void RecordSample(std::string_view label, double duration_ms) {
    Channel().RecordSample(label, duration_ms);
  }
  static void RecordSampleNs(std::string_view label, std::uint64_t duration_ns) {
    if (Channel().AggregateEnabled()) {
      Channel().RecordSample(label, static_cast<double>(duration_ns) / 1'000'000.0);
    }
  }

  static void ResetSummary() { Channel().ResetAggregate(); }
  static std::vector<TraceChannel::AggregateEntry> SummarySnapshot() {
    return Channel().AggregateSnapshot();
  }
  static void WriteSummary(std::FILE* out) { Channel().WriteAggregate(out); }
  // Idempotent: the shutdown path and the atexit backstop both call this.
  static void DumpSummaryOnce() { Channel().DumpAggregateOnce(); }

  class Scope {
   public:
    explicit Scope(std::string_view label) : scope_(Channel(), label) {}

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    TraceScope scope_;
  };

  // Assembles a `Base(key=value,key=value)` label, doing the string work only
  // when the channel is on.
  //
  // A trace label that carries the path/index/size it ran on is what makes a
  // summary row actionable ("which file was slow", not "some file was"), but
  // the concatenation is pure waste in a normal run. Every call site that
  // wanted one was hand-rolling the same `if (Enabled())` guard around a
  // `std::string +=` chain; a missed guard is an allocation per call in
  // production.
  class ScopeLabel {
   public:
    explicit ScopeLabel(std::string_view base) : ScopeLabel(Channel(), base) {}
    // Explicit-channel form. Production code uses the one-argument constructor;
    // this exists so the formatting and the off-path contract are testable
    // without a process-wide env var, rather than through a test backdoor.
    ScopeLabel(TraceChannel& channel, std::string_view base);

    ScopeLabel(const ScopeLabel&) = delete;
    ScopeLabel& operator=(const ScopeLabel&) = delete;

    ScopeLabel& Field(std::string_view key, std::string_view value);
    ScopeLabel& Field(std::string_view key, long long value);

    // Paths are the most common field, so they get an overload rather than
    // leaving each caller to reach for `.native()` (free on POSIX, a wstring
    // that will not convert on Windows) or `.string()` (an allocation on both).
    //
    // Constrained to *exactly* std::filesystem::path: a plain
    // `const std::filesystem::path&` overload is ambiguous against the
    // string_view one for a std::string argument, since path and string_view are
    // both one user-defined conversion away.
    template <typename P,
              typename = std::enable_if_t<std::is_same_v<std::decay_t<P>, std::filesystem::path>>>
    ScopeLabel& Field(std::string_view key, const P& value) {
      return FieldPath(key, value);
    }

    // Empty when the channel is off -- Scope ignores the label in that case, so
    // there is nothing to build.
    std::string_view View();

   private:
    ScopeLabel& FieldPath(std::string_view key, const std::filesystem::path& value);

    std::string text_;
    bool enabled_ = false;
    bool open_ = false;
  };
};

}  // namespace microbrowser::util
