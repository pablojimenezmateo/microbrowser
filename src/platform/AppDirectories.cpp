#include "platform/AppDirectories.h"

#include <cstdlib>
#include <system_error>

namespace microbrowser::platform {

namespace {

constexpr const char* kAppName = "microbrowser";

// An unset variable and an empty one mean the same thing per the XDG spec:
// fall back to the default. An empty string is not a valid base directory.
std::filesystem::path EnvPath(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return {};
  }
  return std::filesystem::path(value);
}

std::filesystem::path HomeRelative(const char* xdg_var, const char* fallback_suffix) {
  const std::filesystem::path from_env = EnvPath(xdg_var);
  if (!from_env.empty()) {
    return from_env / kAppName;
  }
  const std::filesystem::path home = EnvPath("HOME");
  if (home.empty()) {
    // No HOME and no XDG override. Use a relative path rather than guessing at
    // /root or /tmp: it fails visibly in the right directory instead of
    // silently writing browsing history somewhere unexpected.
    return std::filesystem::path(".microbrowser") / fallback_suffix;
  }
  return home / fallback_suffix / kAppName;
}

}  // namespace

AppDirectories::AppDirectories()
    : config_(HomeRelative("XDG_CONFIG_HOME", ".config")),
      data_(HomeRelative("XDG_DATA_HOME", ".local/share")),
      cache_(HomeRelative("XDG_CACHE_HOME", ".cache")) {}

bool AppDirectories::EnsureExist() const {
  bool all_created = true;
  for (const std::filesystem::path* path : {&config_, &data_, &cache_}) {
    std::error_code ec;
    std::filesystem::create_directories(*path, ec);
    if (ec) {
      all_created = false;
      continue;
    }
    // Owner-only. A browser profile contains cookies and history; on a shared
    // machine the default 0755 makes both world-readable.
    std::filesystem::permissions(*path,
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    if (ec) {
      all_created = false;
    }
  }
  return all_created;
}

}  // namespace microbrowser::platform
