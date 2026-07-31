#pragma once

#include <filesystem>

namespace microbrowser::platform {

// Where the browser is allowed to put things on disk.
//
// Centralized because a browser's disk footprint is a privacy surface, not a
// convenience: history, cookies, cache, and downloads each have a different
// retention policy, and scattering the path decisions across features is how a
// "memory-only cache" ends up written to disk anyway. Every persistent
// artifact resolves its location through here, and the architecture lint checks
// that nothing else constructs a profile path.
//
// Honors XDG_CONFIG_HOME / XDG_DATA_HOME / XDG_CACHE_HOME, which is also what
// makes the whole profile relocatable in a test with three env vars.
class AppDirectories {
 public:
  // Resolves the paths once from the environment. Constructed by the
  // application and passed down; deliberately not a singleton, so a test can
  // hold one pointing into a temporary directory.
  AppDirectories();

  // ~/.config/microbrowser — settings the user chose. Backed up by the user;
  // losing it loses preferences.
  const std::filesystem::path& Config() const { return config_; }

  // ~/.local/share/microbrowser — history, bookmarks, cookies, filter lists.
  // Losing it loses browsing state.
  const std::filesystem::path& Data() const { return data_; }

  // ~/.cache/microbrowser — anything reconstructible. The HTTP cache is
  // memory-only by default and does NOT live here; this is for things like
  // compiled filter-list indexes that would otherwise be rebuilt at startup.
  const std::filesystem::path& Cache() const { return cache_; }

  // Create the directories if absent, with owner-only permissions. Returns
  // false if any could not be created; the caller decides whether to run
  // without persistence or exit.
  bool EnsureExist() const;

 private:
  std::filesystem::path config_;
  std::filesystem::path data_;
  std::filesystem::path cache_;
};

}  // namespace microbrowser::platform
