#include "util/TraceChannel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "util/Env.h"
#include "util/Parse.h"
#include "util/TransparentStringHash.h"

namespace microbrowser::util {
namespace {

using Clock = std::chrono::steady_clock;

// One slot per *live* channel. Two exist in the app (startup, perf); the cap
// keeps the thread-local active-scope table a fixed-size array instead of a
// per-thread allocation.
//
// Slots are reclaimed on destruction. A monotonic counter would have been
// simpler, but it burns a slot per channel ever constructed, so a test that
// builds a few throwaway channels silently pushes the next one past the cap and
// disables it -- instrumentation that turns itself off is the worst failure mode
// a tracer can have.
constexpr std::size_t kMaxChannels = 8;
constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

std::mutex& SlotMutex() {
  static std::mutex mutex;
  return mutex;
}

std::array<bool, kMaxChannels>& SlotsInUse() {
  static std::array<bool, kMaxChannels> in_use{};
  return in_use;
}

std::size_t AcquireSlot() {
  std::lock_guard<std::mutex> lock(SlotMutex());
  std::array<bool, kMaxChannels>& in_use = SlotsInUse();
  for (std::size_t i = 0; i < kMaxChannels; ++i) {
    if (!in_use[i]) {
      in_use[i] = true;
      return i;
    }
  }
  return kNoSlot;
}

void ReleaseSlot(std::size_t slot) {
  if (slot >= kMaxChannels) {
    return;
  }
  std::lock_guard<std::mutex> lock(SlotMutex());
  SlotsInUse()[slot] = false;
}

// Innermost live scope per channel on this thread. A plain thread_local array of
// pointers: no allocation, and nesting depth stays per-thread so a background
// scope can no longer shift the indentation of a main-thread one mid-line.
thread_local std::array<TraceScope*, kMaxChannels> g_active_scope{};

// Set on the shell / event-loop thread by MarkTracingMainThread. A plain
// thread_local bool, not a thread-id comparison: it is read on every scope exit.
thread_local bool g_is_main_thread = false;
std::atomic<bool> g_main_thread_marked{false};

double ParseMinimumDurationMs(const char* env_name) {
  const char* value = EnvValue(env_name);
  if (value == nullptr) {
    return 0.0;
  }

  const std::optional<double> parsed = ParseDouble(value);
  if (!parsed.has_value() || !std::isfinite(*parsed)) {
    return 0.0;
  }
  return std::max(0.0, *parsed);
}

double DurationMs(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

constexpr std::string_view kOverflowLabel = "<aggregate-overflow>";

}  // namespace

void MarkTracingMainThread() {
  g_is_main_thread = true;
  g_main_thread_marked.store(true, std::memory_order_relaxed);
}

bool TracingMainThreadIsKnown() { return g_main_thread_marked.load(std::memory_order_relaxed); }

struct TraceChannel::Impl {
  mutable std::mutex mutex;
  Clock::time_point origin = Clock::now();
  std::unordered_map<std::string, AggregateEntry, TransparentStringHash, std::equal_to<>>
      aggregate;
};

TraceChannel::TraceChannel(const char* prefix,
                           const char* stream_env,
                           const char* aggregate_env,
                           const char* min_duration_env)
    : prefix_(prefix == nullptr ? "" : prefix),
      stream_enabled_(EnvFlagEnabled(stream_env)),
      aggregate_enabled_(EnvFlagEnabled(aggregate_env)),
      minimum_duration_ms_(ParseMinimumDurationMs(min_duration_env)) {
  // No other thread can observe this object yet, so relaxed stores below are
  // ordered by the caller's acquisition of it.
  if (!Enabled()) {
    // An off channel needs neither a slot nor state; only its enabled flags are
    // ever read.
    slot_ = kNoSlot;
    return;
  }
  slot_ = AcquireSlot();
  if (slot_ == kNoSlot) {
    // More live channels than the thread-local table can index. Degrade to off
    // rather than corrupting another channel's scope stack.
    stream_enabled_.store(false, std::memory_order_relaxed);
    aggregate_enabled_.store(false, std::memory_order_relaxed);
    std::fprintf(stderr, "[%s] trace channel disabled: more than %zu channels are live\n", prefix_,
                 kMaxChannels);
    return;
  }
  impl_ = std::make_unique<Impl>();
}

TraceChannel::~TraceChannel() {
  // Channels are function-local statics, so this runs at exit. Disarm before
  // freeing: any TraceScope constructed after this point (from a later static
  // destructor) must take the disabled fast path rather than touch `impl_`.
  //
  // The disarm orders this thread against itself; it is not a synchronization
  // mechanism. A worker thread still running when static destruction begins
  // could read `aggregate_enabled_` as true one instruction before the store
  // and then touch a freed `Impl`. The invariant that closes that window is
  // "every worker is joined before main() returns", which belongs to whoever
  // spawns the worker -- see guidelines/security.md on shutdown ordering. It is
  // stated here rather than papered over with a null check, which would move
  // the window without closing it.
  stream_enabled_.store(false, std::memory_order_relaxed);
  aggregate_enabled_.store(false, std::memory_order_relaxed);
  if (slot_ != kNoSlot) {
    g_active_scope[slot_] = nullptr;
    ReleaseSlot(slot_);
    slot_ = kNoSlot;
  }
  impl_.reset();
}

void TraceChannel::Reset() {
  if (impl_ == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->origin = Clock::now();
  }
  g_active_scope[slot_] = nullptr;
}

void TraceChannel::ResetAggregate() {
  if (impl_ == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->aggregate.clear();
}

TraceChannel::Clock::time_point TraceChannel::Origin() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->origin;
}

void TraceChannel::RecordAggregate(std::string_view label, double total_ms, double self_ms,
                                   bool on_main_thread) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto it = impl_->aggregate.find(label);
  if (it == impl_->aggregate.end()) {
    if (impl_->aggregate.size() >= kMaxAggregateLabels) {
      label = kOverflowLabel;
      it = impl_->aggregate.find(label);
    }
    if (it == impl_->aggregate.end()) {
      it = impl_->aggregate.emplace(std::string(label), AggregateEntry{std::string(label)}).first;
    }
  }
  AggregateEntry& entry = it->second;
  ++entry.count;
  entry.total_ms += total_ms;
  entry.self_ms += self_ms;
  if (on_main_thread) {
    entry.main_thread_self_ms += self_ms;
  }
  entry.max_ms = std::max(entry.max_ms, total_ms);
}

void TraceChannel::RecordSample(std::string_view label, double duration_ms) {
  if (!AggregateEnabled() || impl_ == nullptr) {
    return;
  }
  RecordAggregate(label, duration_ms, duration_ms, g_is_main_thread);
}

std::vector<TraceChannel::AggregateEntry> TraceChannel::AggregateSnapshot() const {
  std::vector<AggregateEntry> entries;
  if (impl_ == nullptr) {
    return entries;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    entries.reserve(impl_->aggregate.size());
    for (const auto& [label, entry] : impl_->aggregate) {
      entries.push_back(entry);
    }
  }
  std::sort(entries.begin(), entries.end(), [](const AggregateEntry& a, const AggregateEntry& b) {
    if (a.self_ms != b.self_ms) {
      return a.self_ms > b.self_ms;
    }
    return a.label < b.label;
  });
  return entries;
}

void TraceChannel::WriteAggregate(std::FILE* out) const {
  if (out == nullptr) {
    return;
  }
  const std::vector<AggregateEntry> entries = AggregateSnapshot();
  if (entries.empty()) {
    std::fprintf(out, "[%s] summary: no scopes recorded\n", prefix_);
    std::fflush(out);
    return;
  }

  double self_total = 0.0;
  double main_total = 0.0;
  std::uint64_t call_total = 0;
  for (const AggregateEntry& entry : entries) {
    self_total += entry.self_ms;
    main_total += entry.main_thread_self_ms;
    call_total += entry.count;
  }

  const bool main_known = TracingMainThreadIsKnown();
  std::fprintf(out,
               "[%s] summary: %zu labels, %llu calls, %.2f ms self total, %.2f ms on the main "
               "thread (ranked by self ms)\n",
               prefix_, entries.size(), static_cast<unsigned long long>(call_total), self_total,
               main_total);
  if (!main_known) {
    std::fprintf(out,
                 "[%s] note: no thread called MarkTracingMainThread, so the main-thread column is "
                 "0 everywhere and says nothing\n",
                 prefix_);
  }
  std::fprintf(out, "[%s] %12s %12s %12s %12s %12s %10s  %s\n", prefix_, "self ms", "main ms",
               "total ms", "max ms", "avg ms", "calls", "label");
  for (const AggregateEntry& entry : entries) {
    const double avg_ms = entry.count == 0 ? 0.0 : entry.total_ms / static_cast<double>(entry.count);
    std::fprintf(out, "[%s] %12.3f %12.3f %12.3f %12.3f %12.4f %10llu  %s\n", prefix_,
                 entry.self_ms, entry.main_thread_self_ms, entry.total_ms, entry.max_ms, avg_ms,
                 static_cast<unsigned long long>(entry.count), entry.label.c_str());
  }

  // Second ranking, by main-thread self time only. The first table answers
  // "where does CPU go"; this one answers "what makes the app feel slow", and
  // they disagree often enough to be worth both. Suppressed when the main thread
  // was never marked, since the ordering would then be arbitrary.
  if (main_known && main_total > 0.0) {
    std::vector<const AggregateEntry*> by_main;
    by_main.reserve(entries.size());
    for (const AggregateEntry& entry : entries) {
      if (entry.main_thread_self_ms > 0.0) {
        by_main.push_back(&entry);
      }
    }
    std::sort(by_main.begin(), by_main.end(),
              [](const AggregateEntry* a, const AggregateEntry* b) {
                if (a->main_thread_self_ms != b->main_thread_self_ms) {
                  return a->main_thread_self_ms > b->main_thread_self_ms;
                }
                return a->label < b->label;
              });
    constexpr std::size_t kTopMainThreadRows = 15;
    const std::size_t shown = std::min(kTopMainThreadRows, by_main.size());
    std::fprintf(out, "[%s] top %zu of %zu main-thread scopes (what the user waits on):\n", prefix_,
                 shown, by_main.size());
    for (std::size_t i = 0; i < shown; ++i) {
      const AggregateEntry& entry = *by_main[i];
      std::fprintf(out, "[%s] %12.3f main ms %10llu calls  %s\n", prefix_,
                   entry.main_thread_self_ms, static_cast<unsigned long long>(entry.count),
                   entry.label.c_str());
    }
  }
  std::fflush(out);
}

void TraceChannel::DumpAggregateOnce() {
  if (!AggregateEnabled() || dumped_) {
    return;
  }
  dumped_ = true;
  WriteAggregate(stderr);
}

TraceScope::TraceScope(TraceChannel& channel, std::string_view label) {
  if (!channel.Enabled()) {
    return;
  }

  // Copy the label only on the enabled path. These scopes sit in hot paths (the
  // highlight query path constructs several per call), so an unconditional copy
  // here was a per-scope heap allocation in production with tracing off.
  label_.assign(label);
  channel_ = &channel;
  slot_ = channel.slot_;
  parent_ = g_active_scope[slot_];
  g_active_scope[slot_] = this;
  depth_ = parent_ == nullptr ? 0 : parent_->depth_ + 1;
  start_ = Clock::now();
}

TraceScope::~TraceScope() {
  if (channel_ == nullptr) {
    return;
  }

  const Clock::time_point end = Clock::now();
  const double duration_ms = DurationMs(end - start_);
  g_active_scope[slot_] = parent_;
  if (parent_ != nullptr) {
    parent_->child_ms_ += duration_ms;
  }

  if (channel_->AggregateEnabled()) {
    channel_->RecordAggregate(label_, duration_ms, std::max(0.0, duration_ms - child_ms_),
                              g_is_main_thread);
  }

  if (!channel_->StreamEnabled() || duration_ms < channel_->MinimumDurationMs()) {
    return;
  }

  const double elapsed_ms = DurationMs(end - channel_->Origin());
  std::fprintf(stderr, "[%s] %8.2f ms total | %8.2f ms | %*s%s\n", channel_->prefix_, elapsed_ms,
               duration_ms, depth_ * 2, "", label_.c_str());
  std::fflush(stderr);
}

}  // namespace microbrowser::util
