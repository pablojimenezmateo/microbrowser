#include "util/LoadTimeline.h"

#include <algorithm>
#include <chrono>

#include "util/Env.h"

namespace microbrowser::util {

namespace {

// How many milestones are kept. A page with three hundred subresources would
// otherwise make this grow with the page, which is the one thing an
// instrument must not do -- a diagnostic that costs memory proportional to
// hostile input is a diagnostic that becomes a bug.
constexpr std::size_t kMaxEntries = 4096;

struct State {
  bool enabled = false;
  bool started = false;
  bool dumped = false;
  bool truncated = false;
  std::string what;
  std::chrono::steady_clock::time_point origin{};
  std::vector<LoadTimeline::Entry> entries;
};

State& Get() {
  static State state = [] {
    State created;
    created.enabled = EnvFlagEnabled("MICROBROWSER_LOAD_TIMELINE");
    return created;
  }();
  return state;
}

double SinceOrigin(const State& state) {
  const auto delta = std::chrono::steady_clock::now() - state.origin;
  return std::chrono::duration<double, std::milli>(delta).count();
}

void Record(std::string_view milestone, std::string_view detail) {
  State& state = Get();
  if (!state.enabled || !state.started) {
    return;
  }
  if (state.entries.size() >= kMaxEntries) {
    state.truncated = true;
    return;
  }
  state.entries.push_back(
      LoadTimeline::Entry{std::string(milestone), std::string(detail), SinceOrigin(state)});
}

}  // namespace

bool LoadTimeline::Enabled() { return Get().enabled; }

void LoadTimeline::Begin(std::string_view what) {
  State& state = Get();
  if (!state.enabled) {
    return;
  }
  state.started = true;
  state.dumped = false;
  state.truncated = false;
  state.what = std::string(what);
  state.origin = std::chrono::steady_clock::now();
  state.entries.clear();
  state.entries.push_back(Entry{"navigation.start", std::string(what), 0.0});
}

void LoadTimeline::Mark(std::string_view milestone) { Record(milestone, {}); }

void LoadTimeline::MarkWith(std::string_view milestone, std::string_view detail) {
  Record(milestone, detail);
}

std::vector<LoadTimeline::Entry> LoadTimeline::Snapshot() { return Get().entries; }

void LoadTimeline::DumpOnce(std::FILE* out) {
  State& state = Get();
  if (!state.enabled || state.dumped || state.entries.empty()) {
    return;
  }
  state.dumped = true;
  // Kept in the order they happened rather than sorted: the point of the table
  // is the gap between consecutive rows, and a ranked one cannot show it.
  std::fprintf(out, "[timeline] %s\n", state.what.c_str());
  std::fprintf(out, "[timeline]       at ms      gap ms  milestone\n");
  double previous = 0.0;
  for (const Entry& entry : state.entries) {
    std::fprintf(out, "[timeline] %11.3f %11.3f  %s%s%s\n", entry.at_ms, entry.at_ms - previous,
                 entry.milestone.c_str(), entry.detail.empty() ? "" : " ",
                 entry.detail.c_str());
    previous = entry.at_ms;
  }
  if (state.truncated) {
    std::fprintf(out, "[timeline] (truncated at %zu entries)\n", kMaxEntries);
  }
}

}  // namespace microbrowser::util
