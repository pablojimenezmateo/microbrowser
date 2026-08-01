#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::util {

// Mark the calling thread as the one whose latency the user feels: the shell /
// event-loop thread. Call once, early.
//
// Without it every ranked summary is misleading in the same direction. A
// background tree walk that costs 161 ms of CPU but blocks nobody outranks a
// 32 ms render stall that drops frames, and the table gives the reader no way to
// tell them apart -- so the tool built to find UI pitfalls points at the wrong
// row. Scopes record which side of that line they ran on.
void MarkTracingMainThread();

// True when MarkTracingMainThread() has run somewhere in this process. When it
// has not, the main-thread column is meaningless and the summary says so instead
// of printing zeros.
bool TracingMainThreadIsKnown();

// One env-gated scope-timing sink.
//
// `StartupTrace` and `PerformanceTrace` are thin facades over this. Before this
// type existed they were two byte-identical copies of the same state struct, env
// parsing, `EnsureStarted`, `DurationMs`, and `Scope` body -- so every fix to one
// (thread-local depth, the label-copy allocation, aggregation) had to be made
// twice or silently applied to only one channel.
//
// A channel has two independent output modes:
//
//   stream     one stderr line per scope, indented by nesting depth. This is the
//              original behaviour of both tracers. It is a firehose: the write
//              plus flush happens inside the measured region's parent, so
//              streaming a hot inner scope perturbs what it is measuring.
//
//   aggregate  per-label count / total / self / max, ranked by self time and
//              printed once at exit. This is the mode to use when hunting for a
//              hotspot: no per-scope I/O, and the answer arrives pre-sorted.
//
// Both modes may be on at once. `Enabled()` is true if either is.
class TraceChannel {
 public:
  struct AggregateEntry {
    std::string label;
    std::uint64_t count = 0;
    // Wall time inside the scope, including nested scopes on the same channel.
    double total_ms = 0.0;
    // `total_ms` minus the time attributed to directly nested scopes. This is
    // the column that ranks hotspots: a cheap outer scope that merely contains
    // an expensive one should not outrank the expensive one.
    double self_ms = 0.0;
    // The part of `self_ms` spent on the thread marked by
    // MarkTracingMainThread(). This is the number that costs the user a frame.
    double main_thread_self_ms = 0.0;
    double max_ms = 0.0;
  };

  // Distinct labels retained before overflow is folded into a single bucket.
  // Labels embed paths and counts (`TextViewport::OpenFile(path=...)`), so an
  // unbounded map is a memory leak proportional to session length.
  static constexpr std::size_t kMaxAggregateLabels = 4096;

  // `prefix` is the bracketed line tag ("startup", "perf"). `stream_env` gates
  // the per-scope lines. `aggregate_env` gates the summary and may be null for a
  // channel that has no summary mode. `min_duration_env`, when non-null, names a
  // float env var that suppresses stream lines below that many milliseconds.
  TraceChannel(const char* prefix,
               const char* stream_env,
               const char* aggregate_env,
               const char* min_duration_env);
  ~TraceChannel();

  TraceChannel(const TraceChannel&) = delete;
  TraceChannel& operator=(const TraceChannel&) = delete;

  bool Enabled() const { return StreamEnabled() || AggregateEnabled(); }
  bool StreamEnabled() const { return stream_enabled_.load(std::memory_order_relaxed); }
  bool AggregateEnabled() const { return aggregate_enabled_.load(std::memory_order_relaxed); }
  double MinimumDurationMs() const { return minimum_duration_ms_; }

  // Rebase the elapsed-time origin and clear nesting depth. No-op when the
  // channel is off.
  void Reset();

  // Drop every accumulated label. Used by tests and by callers that want a
  // summary scoped to one phase rather than the whole process.
  void ResetAggregate();

  // Fold an externally measured duration into the same ranked table, as a leaf
  // (self == total). For subsystems that already time themselves in a shape a
  // scope cannot wrap -- the compare pipeline's per-stage nanosecond profile,
  // an LSP round-trip that spans two callbacks. Without this their numbers exist
  // but are only reachable from a bench binary, so a live session cannot say
  // where compare time went. No-op when aggregation is off.
  void RecordSample(std::string_view label, double duration_ms);

  // Snapshot ranked by descending self time. Safe to call from any thread.
  std::vector<AggregateEntry> AggregateSnapshot() const;

  // Write the ranked table to `out`. Idempotent per process for the exit hook:
  // `DumpAggregateOnce` writes at most one summary no matter how many shutdown
  // paths call it.
  void WriteAggregate(std::FILE* out) const;
  void DumpAggregateOnce();

 private:
  friend class TraceScope;

  using Clock = std::chrono::steady_clock;

  // Called by TraceScope on scope exit. `label` is only materialized on the
  // enabled path.
  void RecordAggregate(std::string_view label, double total_ms, double self_ms, bool on_main_thread);
  Clock::time_point Origin() const;

  const char* prefix_ = "";
  // Atomic because the destructor disarms them to tell *other* threads' scopes
  // to take the fast path. A plain bool written on the exiting thread and read
  // on a worker is a data race by the letter of the model, and relaxed ordering
  // is free on every target here -- Enabled() is the hottest read in the whole
  // tracer and this does not change its codegen.
  std::atomic<bool> stream_enabled_{false};
  std::atomic<bool> aggregate_enabled_{false};
  bool dumped_ = false;
  double minimum_duration_ms_ = 0.0;
  // Index into the thread-local active-scope table. Assigned at construction.
  std::size_t slot_ = 0;

  // Pimpl so the mutex and the aggregate map do not appear in every translation
  // unit that only wants to open a scope. Held by unique_ptr rather than a raw
  // pointer: the ownership rule in guidelines/cpp.md has no exemption for
  // "it is only one delete, in one destructor", and the architecture lint
  // enforces that (NoManualHeapOwnership).
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// RAII timer for one region on one channel. Costs a single predictable branch
// when its channel is off; nothing else in the object is touched, so the label
// argument is never copied and no allocation happens on the disabled path.
class TraceScope {
 public:
  TraceScope(TraceChannel& channel, std::string_view label);
  ~TraceScope();

  TraceScope(const TraceScope&) = delete;
  TraceScope& operator=(const TraceScope&) = delete;

 private:
  TraceChannel* channel_ = nullptr;
  TraceScope* parent_ = nullptr;
  // Cached at construction rather than read back through `channel_` on
  // destruction. The channels are function-local statics, so a scope alive
  // during exit can outlive its channel's destructor, which resets the slot to a
  // sentinel -- indexing the thread-local table with it would be an
  // out-of-bounds write, not a no-op.
  std::size_t slot_ = 0;
  std::string label_;
  std::chrono::steady_clock::time_point start_{};
  double child_ms_ = 0.0;
  int depth_ = 0;
};

}  // namespace microbrowser::util
