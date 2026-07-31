#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::app {

struct AppStartupOptions {
  std::string url;
  int width = 1280;
  int height = 800;

  // Set when the command line asked for output instead of a browser. The caller
  // prints `message` and exits with `exit_code`; parsing never prints or exits
  // on its own, which is what makes it testable.
  bool should_exit = false;
  int exit_code = 0;
  std::string message;
};

// Parses argv[1..]. Unknown flags are an error rather than being ignored: a
// silently dropped `--private` or `--proxy` is a privacy failure, not a
// usability quirk, so the policy is set now while the flag set is empty.
AppStartupOptions ParseStartupOptions(std::span<const std::string_view> arguments);

// Convenience for main(). Skips argv[0].
AppStartupOptions ParseStartupOptions(int argc, char** argv);

std::string UsageText();

}  // namespace microbrowser::app
