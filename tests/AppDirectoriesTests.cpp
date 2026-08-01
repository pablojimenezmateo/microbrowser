#include <filesystem>
#include <vector>

#include "TestSupport.h"
#include "platform/AppDirectories.h"

// First automated coverage of anything in src/platform. It is reachable without
// a window system because AppDirectories deliberately takes its paths from the
// environment, which is also what makes the whole profile relocatable into a
// temp directory in four lines.
//
// One thing here is *not* covered, and it is the security property the code was
// changed for: that the directory is never world-readable at any instant, not
// merely that it ends up owner-only. The previous implementation
// (create_directories, then chmod) also ends up owner-only and passes every
// assertion below -- what it had was a window in between. Observing that window
// needs a second process racing the first, which is a heavier fixture than the
// bug warrants. The end-state assertions are still worth having: they catch the
// mode being dropped entirely, which is the more likely regression.

namespace microbrowser::tests {

namespace {

std::filesystem::perms PermissionsOf(const std::filesystem::path& path) {
  return std::filesystem::status(path).permissions() & std::filesystem::perms::mask;
}

bool ReadableByOthers(const std::filesystem::path& path) {
  constexpr std::filesystem::perms kOthers =
      std::filesystem::perms::group_all | std::filesystem::perms::others_all;
  return (PermissionsOf(path) & kOthers) != std::filesystem::perms::none;
}

}  // namespace

void RegisterAppDirectoriesTests(std::vector<TestCase>& tests) {
  AddTest(tests, "AppDirectories/XdgOverridesRelocateTheWholeProfile", [] {
    const TemporaryDirectory root;
    const ScopedEnvVar config("XDG_CONFIG_HOME", (root.Path() / "config").string());
    const ScopedEnvVar data("XDG_DATA_HOME", (root.Path() / "data").string());
    const ScopedEnvVar cache("XDG_CACHE_HOME", (root.Path() / "cache").string());

    const platform::AppDirectories directories;
    ExpectEqString(directories.Config().string(), (root.Path() / "config/microbrowser").string(),
                   "XDG_CONFIG_HOME must place the profile under it");
    ExpectEqString(directories.Data().string(), (root.Path() / "data/microbrowser").string(),
                   "XDG_DATA_HOME must place the profile under it");
    ExpectEqString(directories.Cache().string(), (root.Path() / "cache/microbrowser").string(),
                   "XDG_CACHE_HOME must place the profile under it");
  });

  AddTest(tests, "AppDirectories/CreatedDirectoriesAreOwnerOnly", [] {
    const TemporaryDirectory root;
    const ScopedEnvVar config("XDG_CONFIG_HOME", (root.Path() / "config").string());
    const ScopedEnvVar data("XDG_DATA_HOME", (root.Path() / "data").string());
    const ScopedEnvVar cache("XDG_CACHE_HOME", (root.Path() / "cache").string());

    const platform::AppDirectories directories;
    Expect(directories.EnsureExist(), "EnsureExist must succeed under a writable temp root");

    for (const std::filesystem::path& path :
         {directories.Config(), directories.Data(), directories.Cache()}) {
      Expect(std::filesystem::is_directory(path), path.string() + " was not created");
      Expect(!ReadableByOthers(path),
             path.string() + " is readable by group or other; a profile holds cookies and "
                             "history, and on a shared machine that is the whole attack");
    }
  });

  AddTest(tests, "AppDirectories/ExistingWorldReadableProfileIsTightened", [] {
    const TemporaryDirectory root;
    const ScopedEnvVar config("XDG_CONFIG_HOME", (root.Path() / "config").string());
    const ScopedEnvVar data("XDG_DATA_HOME", (root.Path() / "data").string());
    const ScopedEnvVar cache("XDG_CACHE_HOME", (root.Path() / "cache").string());

    const platform::AppDirectories directories;
    std::filesystem::create_directories(directories.Data());
    std::filesystem::permissions(directories.Data(), std::filesystem::perms::owner_all |
                                                         std::filesystem::perms::group_read |
                                                         std::filesystem::perms::others_read);
    Expect(ReadableByOthers(directories.Data()), "test setup failed to loosen the directory");

    Expect(directories.EnsureExist(), "EnsureExist must succeed over an existing directory");
    Expect(!ReadableByOthers(directories.Data()),
           "a profile left world-readable by an older build must be tightened, not accepted; "
           "otherwise the fix only protects installs that never ran the old code");
  });

  AddTest(tests, "AppDirectories/EnsureExistIsIdempotent", [] {
    const TemporaryDirectory root;
    const ScopedEnvVar config("XDG_CONFIG_HOME", (root.Path() / "config").string());
    const ScopedEnvVar data("XDG_DATA_HOME", (root.Path() / "data").string());
    const ScopedEnvVar cache("XDG_CACHE_HOME", (root.Path() / "cache").string());

    const platform::AppDirectories directories;
    Expect(directories.EnsureExist(), "first call must succeed");
    Expect(directories.EnsureExist(), "a second call must succeed rather than fail on EEXIST");
    Expect(!ReadableByOthers(directories.Cache()), "permissions must survive a second call");
  });
}

}  // namespace microbrowser::tests
