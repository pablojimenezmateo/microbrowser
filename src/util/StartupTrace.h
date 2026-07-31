#pragma once

#include <cstdio>
#include <string_view>
#include <vector>

#include "util/TraceChannel.h"

namespace microbrowser::util {

// Startup-phase scope tracer.
//
//   MICROBROWSER_STARTUP_TRACE=1    stream one stderr line per scope (unfiltered:
//                               startup scopes are few and all of them matter).
//   MICROBROWSER_STARTUP_SUMMARY=1  print a self-time-ranked table instead, which is
//                               what you want once the same scope runs per file
//                               or per plugin rather than once.
class StartupTrace {
 public:
  static TraceChannel& Channel();

  static bool Enabled() { return Channel().Enabled(); }
  static bool SummaryEnabled() { return Channel().AggregateEnabled(); }

  // Rebase the elapsed-time origin so "ms total" is measured from the start of
  // Application::Initialize rather than from first use.
  static void Reset() { Channel().Reset(); }

  static void ResetSummary() { Channel().ResetAggregate(); }
  static std::vector<TraceChannel::AggregateEntry> SummarySnapshot() {
    return Channel().AggregateSnapshot();
  }
  static void WriteSummary(std::FILE* out) { Channel().WriteAggregate(out); }
  static void DumpSummaryOnce() { Channel().DumpAggregateOnce(); }

  class Scope {
   public:
    explicit Scope(std::string_view label) : scope_(Channel(), label) {}

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    TraceScope scope_;
  };
};

}  // namespace microbrowser::util
