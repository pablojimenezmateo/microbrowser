#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::tests {

struct TestCase {
  std::string name;
  // std::function rather than a bare function pointer so parameterized suites
  // can register per-case closures — one ctest case per architecture rule, for
  // instance, which is what lets the lint shard across cores like everything
  // else.
  std::function<void()> run;
};

void AddTest(std::vector<TestCase>& tests, std::string_view name, std::function<void()> run);

// Throws on failure; the runner catches and reports. Deliberately one
// primitive: a rich matcher library is a maintenance surface, and a good
// message from the call site beats a generated one.
void Expect(bool condition, std::string_view message);

// Formats both sides into the failure message, which is the one thing plain
// Expect genuinely cannot do well.
void ExpectEqInt(long long actual, long long expected, std::string_view message);
void ExpectEqString(std::string_view actual, std::string_view expected, std::string_view message);

// The repository root, baked in at configure time. Tests that read source files
// (the architecture lint) need it, and deriving it from argv[0] breaks the
// moment a build directory moves.
std::filesystem::path SourceRoot();

std::string ReadFile(const std::filesystem::path& path);
void WriteFile(const std::filesystem::path& path, std::string_view content);

class TemporaryDirectory {
 public:
  TemporaryDirectory();
  ~TemporaryDirectory();

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
};

// Sets an environment variable for the lifetime of the object and restores the
// previous value (or unsets it) afterward.
class ScopedEnvVar {
 public:
  ScopedEnvVar(std::string name, std::string_view value);
  ~ScopedEnvVar();

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

 private:
  std::string name_;
  std::string previous_;
  bool had_previous_ = false;
};

}  // namespace microbrowser::tests
