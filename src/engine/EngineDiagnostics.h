#pragma once

#include <string>
#include <vector>

namespace microbrowser::engine {

class Engine;

// What this document's policy refused, in order. Local and never sent -- a
// violation report is an outbound request the user did not cause.
const std::vector<std::string>& CspViolations(const Engine& engine);

}  // namespace microbrowser::engine
