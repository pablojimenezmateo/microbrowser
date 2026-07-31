#include "app/AppStartupOptions.h"

#include <cstddef>
#include <optional>

#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microbrowser::app {

namespace {

// Reject nonsense window geometry at the boundary rather than letting a
// zero-size or absurd canvas allocation happen deeper in.
constexpr int kMinWindowExtent = 160;
constexpr int kMaxWindowExtent = 16384;

bool TakeExtent(std::span<const std::string_view> arguments,
                std::size_t& index,
                std::string_view flag,
                int& out_value,
                AppStartupOptions& options) {
  if (index + 1 >= arguments.size()) {
    options.should_exit = true;
    options.exit_code = 2;
    options.message = std::string(flag) + " requires a value\n" + UsageText();
    return false;
  }
  ++index;
  const std::optional<int> parsed = util::ParseInt(arguments[index]);
  if (!parsed.has_value() || *parsed < kMinWindowExtent || *parsed > kMaxWindowExtent) {
    options.should_exit = true;
    options.exit_code = 2;
    options.message = std::string(flag) + " must be an integer between " +
                      std::to_string(kMinWindowExtent) + " and " +
                      std::to_string(kMaxWindowExtent) + "\n" + UsageText();
    return false;
  }
  out_value = *parsed;
  return true;
}

}  // namespace

std::string UsageText() {
  return "usage: microbrowser [options] [url]\n"
         "\n"
         "  --width <px>    initial window width (default 1280)\n"
         "  --height <px>   initial window height (default 800)\n"
         "  --version       print version and exit\n"
         "  --help          print this message and exit\n";
}

AppStartupOptions ParseStartupOptions(std::span<const std::string_view> arguments) {
  AppStartupOptions options;

  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::string_view argument = arguments[i];

    if (argument == "--help" || argument == "-h") {
      options.should_exit = true;
      options.exit_code = 0;
      options.message = UsageText();
      return options;
    }
    if (argument == "--version") {
      options.should_exit = true;
      options.exit_code = 0;
      options.message = "microbrowser 0.1.0\n";
      return options;
    }
    if (argument == "--width") {
      if (!TakeExtent(arguments, i, "--width", options.width, options)) {
        return options;
      }
      continue;
    }
    if (argument == "--height") {
      if (!TakeExtent(arguments, i, "--height", options.height, options)) {
        return options;
      }
      continue;
    }
    if (util::StartsWith(argument, "-")) {
      options.should_exit = true;
      options.exit_code = 2;
      options.message = "unknown option: " + std::string(argument) + "\n" + UsageText();
      return options;
    }
    if (!options.url.empty()) {
      options.should_exit = true;
      options.exit_code = 2;
      options.message = "at most one url may be given\n" + UsageText();
      return options;
    }
    options.url = std::string(argument);
  }

  return options;
}

AppStartupOptions ParseStartupOptions(int argc, char** argv) {
  std::vector<std::string_view> arguments;
  if (argc > 1 && argv != nullptr) {
    arguments.reserve(static_cast<std::size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
      arguments.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
  }
  return ParseStartupOptions(arguments);
}

}  // namespace microbrowser::app
