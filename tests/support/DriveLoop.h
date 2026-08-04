#pragma once

#include <utility>
#include <vector>

#include "engine/Engine.h"
#include "engine/Loader.h"

namespace microbrowser::tests {

// Turns the loop's crank for a test.
//
// The host loop does this between blocking waits. A test has no window to wait
// on and a canned transport never blocks, so turning the crank directly is the
// whole of it -- which is also the argument for `HasRunnableWork` existing at
// all.
//
// Bounded rather than `while (loading)`: a load that stops making progress must
// fail a test rather than hang the suite.
inline constexpr int kMaxDriveTurns = 10000;

inline void RunEngineToIdle(engine::Engine& engine) {
  for (int turn = 0; turn < kMaxDriveTurns && engine.IsLoading(); ++turn) {
    if (!engine.Advance() && !engine.HasRunnableWork()) {
      break;
    }
  }
}

// Same, for a loader driven directly. Returns everything that completed, in
// completion order.
inline std::vector<engine::Loader::Completion> RunLoaderToIdle(engine::Loader& loader) {
  std::vector<engine::Loader::Completion> out;
  for (int turn = 0; turn < kMaxDriveTurns && !loader.IsIdle(); ++turn) {
    loader.Advance(turn);
    for (engine::Loader::Completion& completion : loader.TakeCompletions()) {
      out.push_back(std::move(completion));
    }
  }
  return out;
}

// The common case: one request, started and driven to its answer.
inline engine::Loader::Result RunOneRequest(engine::Loader& loader,
                                            engine::Loader::RequestId id) {
  for (engine::Loader::Completion& completion : RunLoaderToIdle(loader)) {
    if (completion.id == id) {
      return std::move(completion.result);
    }
  }
  return engine::Loader::Result{};
}

}  // namespace microbrowser::tests
