#include "TestSupport.h"

#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifndef MICROBROWSER_SOURCE_ROOT
#error "MICROBROWSER_SOURCE_ROOT must be defined by the build"
#endif

namespace microbrowser::tests {

namespace {

// Unique per directory within a process, so two fixtures in the same shard
// cannot collide. Cross-process uniqueness comes from the pid.
std::atomic<unsigned> g_temp_counter{0};

}  // namespace

void AddTest(std::vector<TestCase>& tests, std::string_view name, std::function<void()> run) {
  tests.push_back(TestCase{std::string(name), std::move(run)});
}

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void ExpectEqInt(long long actual, long long expected, std::string_view message) {
  if (actual != expected) {
    std::ostringstream out;
    out << message << " (actual " << actual << ", expected " << expected << ")";
    throw std::runtime_error(out.str());
  }
}

void ExpectEqString(std::string_view actual, std::string_view expected, std::string_view message) {
  if (actual != expected) {
    std::ostringstream out;
    out << message << " (actual \"" << actual << "\", expected \"" << expected << "\")";
    throw std::runtime_error(out.str());
  }
}

std::filesystem::path SourceRoot() {
  return std::filesystem::path(MICROBROWSER_SOURCE_ROOT);
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("could not read " + path.string());
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

void WriteFile(const std::filesystem::path& path, std::string_view content) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("could not write " + path.string());
  }
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
}

TemporaryDirectory::TemporaryDirectory() {
  const unsigned serial = g_temp_counter.fetch_add(1);
  std::ostringstream name;
  name << "microbrowser-test-" << static_cast<long long>(::getpid()) << '-' << serial;
  path_ = std::filesystem::temp_directory_path() / name.str();

  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
  std::filesystem::create_directories(path_, ec);
  if (ec) {
    throw std::runtime_error("could not create " + path_.string());
  }
}

TemporaryDirectory::~TemporaryDirectory() {
  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
}

ScopedEnvVar::ScopedEnvVar(std::string name, std::string_view value) : name_(std::move(name)) {
  if (const char* existing = std::getenv(name_.c_str()); existing != nullptr) {
    previous_ = existing;
    had_previous_ = true;
  }
  ::setenv(name_.c_str(), std::string(value).c_str(), 1);
}

ScopedEnvVar::~ScopedEnvVar() {
  if (had_previous_) {
    ::setenv(name_.c_str(), previous_.c_str(), 1);
  } else {
    ::unsetenv(name_.c_str());
  }
}

}  // namespace microbrowser::tests
