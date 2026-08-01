#include "platform/AppDirectories.h"

#include <sys/stat.h>

#include <cerrno>
#include <system_error>

#include "util/Env.h"

namespace microbrowser::platform {

namespace {

constexpr const char* kAppName = "microbrowser";

// An unset variable and an empty one mean the same thing per the XDG spec:
// fall back to the default. util::EnvValue already collapses the two.
std::filesystem::path EnvPath(const char* name) {
  const char* value = util::EnvValue(name);
  return value == nullptr ? std::filesystem::path{} : std::filesystem::path(value);
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

// Create `path` such that it is never, at any instant, readable by anyone else.
//
// The obvious spelling -- create_directories() then permissions() -- leaves a
// window in which a directory holding cookies and history exists as 0755. On a
// shared machine that window is the whole attack: another user opens the
// directory during it and keeps the descriptor, and tightening the mode
// afterwards does not revoke an open descriptor. This is the same reasoning
// that puts close-on-exec on the creating call rather than a follow-up fcntl
// (see guidelines/security.md).
//
// So the mode goes in the mkdir() call itself. The XDG base directory above it
// (~/.config, ~/.local/share, ~/.cache) is created with the system default,
// because it is shared with every other application and is not ours to tighten.
bool CreateOwnerOnlyDirectory(const std::filesystem::path& path) {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return false;
    }
  }

  const bool created = ::mkdir(path.c_str(), S_IRWXU) == 0;
  if (!created && errno != EEXIST) {
    return false;
  }

  // Unconditional, for two reasons: umask subtracts from the mkdir mode and
  // could have taken the owner-execute bit with it, and a profile created by an
  // older build may still be 0755. Applied to a directory that was already
  // owner-only in the created case, so it opens no window of its own.
  std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, ec);
  return !ec;
}

}  // namespace

AppDirectories::AppDirectories()
    : config_(HomeRelative("XDG_CONFIG_HOME", ".config")),
      data_(HomeRelative("XDG_DATA_HOME", ".local/share")),
      cache_(HomeRelative("XDG_CACHE_HOME", ".cache")) {}

bool AppDirectories::EnsureExist() const {
  bool all_created = true;
  for (const std::filesystem::path* path : {&config_, &data_, &cache_}) {
    if (!CreateOwnerOnlyDirectory(*path)) {
      all_created = false;
    }
  }
  return all_created;
}

}  // namespace microbrowser::platform
