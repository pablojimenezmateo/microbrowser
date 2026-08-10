#include "wpt/Expectations.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace microbrowser::wpt {
namespace {

// The top-level WPT directory a test belongs to, which is the file its
// expectation lives in.
std::string TopLevel(const std::string& url_path) {
  const std::size_t slash = url_path.find('/');
  return slash == std::string::npos ? url_path : url_path.substr(0, slash);
}

std::string Trim(std::string value) {
  while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
    value.pop_back();
  }
  std::size_t start = 0;
  while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
    ++start;
  }
  return value.substr(start);
}

std::string Serialize(const std::string& url_path, const TestExpectation& expectation) {
  std::string text = "[" + url_path + "]\n";
  if (expectation.disabled) {
    text += "disabled=" + expectation.disabled_reason + "\n";
  }
  if (expectation.harness != "OK") {
    text += "harness=" + expectation.harness + "\n";
  }
  for (const auto& [name, status] : expectation.subtests) {
    text += status + "=" + name + "\n";
  }
  return text;
}

}  // namespace

void ExpectationStore::Load(const std::string& directory) {
  std::error_code code;
  if (!std::filesystem::is_directory(directory, code)) {
    return;
  }
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(directory, code)) {
    if (!entry.is_regular_file(code) || entry.path().extension() != ".txt") {
      continue;
    }
    std::ifstream stream(entry.path());
    std::string line;
    std::string current;
    while (std::getline(stream, line)) {
      line = Trim(line);
      if (line.empty() || line[0] == '#') {
        continue;
      }
      if (line.front() == '[' && line.back() == ']') {
        current = line.substr(1, line.size() - 2);
        tests_[current];  // default-construct: listed but all-PASS is legal
        continue;
      }
      if (current.empty()) {
        continue;
      }
      const std::size_t equals = line.find('=');
      if (equals == std::string::npos) {
        continue;
      }
      const std::string key = line.substr(0, equals);
      const std::string value = line.substr(equals + 1);
      TestExpectation& expectation = tests_[current];
      if (key == "harness") {
        expectation.harness = value;
      } else if (key == "disabled") {
        expectation.disabled = true;
        expectation.disabled_reason = value;
      } else {
        // Anything else is a subtest status, and the value is the name -- which
        // is why the status is the key: a subtest name can contain '=' and a
        // status never can.
        expectation.subtests[value] = key;
      }
    }
  }
}

const TestExpectation* ExpectationStore::Find(const std::string& url_path) const {
  const auto found = tests_.find(url_path);
  return found == tests_.end() ? nullptr : &found->second;
}

void ExpectationStore::Set(const std::string& url_path, TestExpectation expectation) {
  if (expectation.harness == "OK" && expectation.subtests.empty() && !expectation.disabled) {
    tests_.erase(url_path);
    return;
  }
  tests_[url_path] = std::move(expectation);
}

bool ExpectationStore::Save(const std::string& directory, std::string* error) const {
  std::error_code code;
  std::filesystem::create_directories(directory, code);
  std::map<std::string, std::string> files;
  for (const auto& [url_path, expectation] : tests_) {
    files[TopLevel(url_path)] += Serialize(url_path, expectation);
  }
  // A file that is now empty must be removed rather than left stale: an
  // expectation file that still exists after its last failure is fixed reads as
  // "these still fail".
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(directory, code)) {
    if (entry.is_regular_file(code) && entry.path().extension() == ".txt" &&
        files.find(entry.path().stem().string()) == files.end()) {
      std::filesystem::remove(entry.path(), code);
    }
  }
  for (const auto& [name, contents] : files) {
    const std::filesystem::path path = std::filesystem::path(directory) / (name + ".txt");
    std::ifstream existing(path);
    if (existing) {
      std::ostringstream buffer;
      buffer << existing.rdbuf();
      if (buffer.str() == contents) {
        continue;
      }
    }
    std::ofstream stream(path, std::ios::trunc);
    if (!stream) {
      if (error != nullptr) {
        *error = "could not write " + path.string();
      }
      return false;
    }
    stream << contents;
  }
  return true;
}

}  // namespace microbrowser::wpt
