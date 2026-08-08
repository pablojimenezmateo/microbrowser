#pragma once

#include <string>
#include <vector>

namespace microbrowser::engine {

class Engine;

// What this document's policy refused, in order. Local and never sent -- a
// violation report is an outbound request the user did not cause.
const std::vector<std::string>& CspViolations(const Engine& engine);

// One layout-and-paint after post-load script has settled. The snapshot tool
// calls this before writing a frame so a hoisted feed is on the display list
// the PPM is built from, without re-entering the engine from inside `Advance`.
void SettleForSnapshot(Engine& engine);

}  // namespace microbrowser::engine
